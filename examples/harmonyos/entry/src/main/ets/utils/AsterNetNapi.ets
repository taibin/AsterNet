/**
 * AsterNet NAPI 桥接 — ArkTS 封装层
 *
 * 使用 native 模块调用 C ABI，提供类型安全的 TS 接口。
 */

// NAPI 模块声明（对应 napi_init.cpp 中的 nm_modname = "asternet_napi"）
import asternet_napi from 'libasternet_napi.so'

/** 协议策略 */
export enum Policy {
  AUTO = 0,
  HTTP_1_1_ONLY = 1,
  HTTP_2_ONLY = 2,
  HTTP_3_ONLY = 3,
  PREFER_HTTP_3 = 4,
  PREFER_HTTP_2 = 5,
}

/** 响应模型 */
export interface AsterNetResponse {
  result: number
  error: string
  status: number
  protocol: number
  degraded: number
  bodySize: number
  dnsMs: number
  connectMs: number
  tlsMs: number
  ttfbMs: number
  totalMs: number
  body: string
}

/** 创建 Client */
export function createClient(enableH3: boolean = true): number {
  return asternet_napi.createClient(enableH3)
}

/** 发送同步请求 */
export function request(
  host: string, port: number, method: string, path: string,
  policy: Policy, headers: string = '', body: string = '',
  timeoutMs: number = 12000, idempotent: boolean = true
): AsterNetResponse {
  const json = asternet_napi.request(
    host, port, method, path, policy, headers, body, timeoutMs, idempotent
  )
  return JSON.parse(json) as AsterNetResponse
}

/** 获取版本 */
export function version(): string {
  return asternet_napi.version()
}

/** 销毁 Client */
export function destroy(): void {
  asternet_napi.destroy()
}

/** 协议名称 */
export function protocolName(proto: number): string {
  switch (proto) {
    case 1: return 'HTTP/1.1'
    case 2: return 'HTTP/2'
    case 3: return 'HTTP/3'
    default: return 'UNKNOWN'
  }
}

/** 策略名称 */
export function policyName(policy: Policy): string {
  switch (policy) {
    case Policy.HTTP_1_1_ONLY: return 'HTTP/1.1 only'
    case Policy.HTTP_2_ONLY: return 'HTTP/2 only'
    case Policy.HTTP_3_ONLY: return 'HTTP/3 only'
    case Policy.PREFER_HTTP_3: return 'Prefer HTTP/3'
    case Policy.PREFER_HTTP_2: return 'Prefer HTTP/2'
    default: return 'AUTO'
  }
}
