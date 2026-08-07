// Package main 是 asternet 的对端测试服务器。
//
// 用途：为基于 xquic 的 C++ 客户端（asternet）提供 HTTP/1.1、HTTP/2、HTTP/3
// 三协议对端，验证自研网络库端到端连通性。同一 handler 同时挂在 H3 与 H1/H2 两个监听器上，
// 便于对比同一请求在三种协议下的行为。
//
// 端口约定（与 docs/POC_PROGRESS.md、tests/test_quic_engine.cpp 对齐）：
//   - :8443  HTTP/3 over QUIC（UDP），ALPN = h3
//   - :9443  HTTP/1.1 + HTTP/2 over TCP（TLS），ALPN = h2, http/1.1（自动协商降级）
//
// 启动：go run main.go
//   首次运行会在当前目录自动生成自签名证书 cert.pem / key.pem（SAN: localhost, 127.0.0.1, ::1）。
//   curl 加 -k 跳过证书校验；xquic 客户端 POC 阶段同样跳过 cert_verify。
package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/quic-go/quic-go/http3"
)

const (
	certFile = "cert.pem"
	keyFile  = "key.pem"
	h3Addr   = ":8443" // HTTP/3 over QUIC（UDP）
	h12Addr  = ":9443" // HTTP/1.1 + HTTP/2 over TCP（TLS）
)

func main() {
	// 1. 自签名证书：首次运行自动生成，保证 `go run main.go` 一条命令即可启动。
	if err := genSelfSignedCert(); err != nil {
		log.Fatalf("生成自签名证书失败: %v", err)
	}
	log.Printf("证书就绪: %s / %s（自签名，客户端用 -k 跳过校验）", certFile, keyFile)

	// 2. 统一路由：两个监听器共用同一 handler，便于三协议行为对比。
	mux := http.NewServeMux()
	mux.HandleFunc("/", handleInfo)
	// 与 tests/test_quic_engine.cpp 中 "/index.html" 路径对齐，复用同一响应。
	mux.HandleFunc("/index.html", handleInfo)

	// 3. H1/H2 TCP 监听器（:9443）。
	//    NextProtos 同时声明 h2 与 http/1.1，标准库 net/http 会据此在 TLS 握手时协商协议：
	//    curl --http2 → h2；裸 curl → http/1.1。
	h12 := &http.Server{
		Addr:    h12Addr,
		Handler: mux,
		TLSConfig: &tls.Config{
			NextProtos: []string{"h2", "http/1.1"},
			MinVersion: tls.VersionTLS12,
		},
		ReadHeaderTimeout: 10 * time.Second,
		// 防空闲连接长期挂死（本地测试虽风险低，但规范起见仍设上限）。
		IdleTimeout: 120 * time.Second,
	}
	go func() {
		log.Printf("[H1/H2] 监听 https://localhost%s (TCP, ALPN: h2, http/1.1)", h12Addr)
		if err := h12.ListenAndServeTLS(certFile, keyFile); err != nil {
			log.Fatalf("[H1/H2] 启动失败: %v", err)
		}
	}()

	// 4. H3 QUIC 监听器（:8443）。
	//    HTTP/3 强制 TLS 1.3；ALPN 由 quic-go http3 内部固定为 "h3"，无需手动声明。
	h3 := &http3.Server{
		Addr:    h3Addr,
		Handler: mux,
	}
	go func() {
		log.Printf("[H3]   监听 https://localhost%s (UDP/QUIC, ALPN: h3, TLS 1.3)", h3Addr)
		if err := h3.ListenAndServeTLS(certFile, keyFile); err != nil {
			log.Fatalf("[H3] 启动失败: %v", err)
		}
	}()

	// 5. 等待退出信号，优雅关闭。
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	log.Printf("收到退出信号，正在关闭服务器...")
	if err := h12.Close(); err != nil {
		log.Printf("[H1/H2] 关闭: %v", err)
	}
	if err := h3.Close(); err != nil {
		log.Printf("[H3] 关闭: %v", err)
	}
}

// respInfo 是 handleInfo 返回的 JSON 结构，含本次请求协商出的协议与关键元信息，
// 便于客户端直观判断 H1/H2/H3 是否协商成功。
type respInfo struct {
	Proto      string              `json:"proto"`       // HTTP/1.1 | HTTP/2.0 | HTTP/3.0
	ALPN       string              `json:"alpn"`        // http/1.1 | h2 | h3
	Method     string              `json:"method"`
	Host       string              `json:"host"`
	Path       string              `json:"path"`
	Header     map[string][]string `json:"header"`
	RemoteAddr string              `json:"remote_addr"`
	ServerTime string              `json:"server_time"`
	Message    string              `json:"message"`
}

func handleInfo(w http.ResponseWriter, r *http.Request) {
	alpn := ""
	if r.TLS != nil && r.TLS.NegotiatedProtocol != "" {
		alpn = r.TLS.NegotiatedProtocol
	}
	info := respInfo{
		Proto:      r.Proto,
		ALPN:       alpn,
		Method:     r.Method,
		Host:       r.Host,
		Path:       r.URL.Path,
		Header:     r.Header,
		RemoteAddr: r.RemoteAddr,
		ServerTime: time.Now().Format(time.RFC3339Nano),
		Message:    "AsterNet test server ok",
	}
	// Alt-Svc：提示 TCP 客户端可升级到 H3（curl --http3 直连 8443 无需此头，仅作语义示意）。
	w.Header().Set("Alt-Svc", `h3=":8443"; ma=86400`)
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	enc := json.NewEncoder(w)
	enc.SetIndent("", "  ")
	if err := enc.Encode(info); err != nil {
		log.Printf("写响应失败: %v", err)
	}
}

// genSelfSignedCert 在 cert.pem/key.pem 不存在时生成自签名 ECDSA-P256 证书。
// SAN 同时覆盖 localhost 与 127.0.0.1/::1，使 `curl https://localhost:...` 即便不带 -k
// 也能通过 SAN 校验（带 -k 则完全跳过）。
func genSelfSignedCert() error {
	if fileExists(certFile) && fileExists(keyFile) {
		return nil
	}
	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return fmt.Errorf("生成 ECDSA 私钥: %w", err)
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return fmt.Errorf("生成序列号: %w", err)
	}
	tmpl := x509.Certificate{
		SerialNumber: serial,
		Subject: pkix.Name{
			Organization: []string{"asternet"},
			CommonName:   "localhost",
		},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().Add(365 * 24 * time.Hour),
		KeyUsage:              x509.KeyUsageDigitalSignature,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames:              []string{"localhost"},
		IPAddresses:           []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")},
		BasicConstraintsValid: true,
	}
	der, err := x509.CreateCertificate(rand.Reader, &tmpl, &tmpl, &priv.PublicKey, priv)
	if err != nil {
		return fmt.Errorf("签发证书: %w", err)
	}
	// 写证书
	if err := writePEM(certFile, "CERTIFICATE", der, 0644); err != nil {
		return err
	}
	// 写私钥
	keyDer, err := x509.MarshalECPrivateKey(priv)
	if err != nil {
		return fmt.Errorf("编码私钥: %w", err)
	}
	if err := writePEM(keyFile, "EC PRIVATE KEY", keyDer, 0600); err != nil {
		return err
	}
	log.Printf("已生成自签名证书（SAN: localhost, 127.0.0.1, ::1；有效期 365 天）")
	return nil
}

func writePEM(path, typ string, der []byte, mode os.FileMode) error {
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, mode)
	if err != nil {
		return fmt.Errorf("创建 %s: %w", path, err)
	}
	// 写入失败或关闭失败都要返回错误，确保文件落盘完整。
	if err := pem.Encode(f, &pem.Block{Type: typ, Bytes: der}); err != nil {
		_ = f.Close()
		return fmt.Errorf("写入 %s: %w", path, err)
	}
	if err := f.Close(); err != nil {
		return fmt.Errorf("关闭 %s: %w", path, err)
	}
	return nil
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}
