// 临时 HTTP/3 客户端：验证 server 的 UDP:8443 H3 真能收发。验证后可删除此目录。
package main

import (
	"crypto/tls"
	"fmt"
	"io"
	"net/http"

	"github.com/quic-go/quic-go/http3"
)

func main() {
	tr := &http3.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true}, // 自签名证书，跳过校验
	}
	client := &http.Client{Transport: tr}
	resp, err := client.Get("https://localhost:8443/")
	if err != nil {
		fmt.Printf("H3 请求失败: %v\n", err)
		return
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	alpn := ""
	if resp.TLS != nil {
		alpn = resp.TLS.NegotiatedProtocol
	}
	fmt.Printf("proto=%s  alpn=%s  status=%d\n", resp.Proto, alpn, resp.StatusCode)
	fmt.Printf("body=%s\n", string(body))
}
