# AsterNet C ABI 设计

`include/asternet/asternet.h` 是 Android JNI、iOS ObjC++ 和 C++ 核心之间的稳定契约。修改结构体布局、枚举或导出函数时，必须同步更新本文并调整 ABI 主版本。

## 命名与边界

- 导出函数使用 `asternet_` 前缀，类型使用 `asternet_*_t`，枚举常量使用 `ASTERNET_*`。
- `asternet_client_t` 是不透明句柄，端侧不能解引用或依赖 C++ 布局。
- ABI 只传 POD、UTF-8 字符串指针、字节数组和数组长度，不跨边界传 STL 类型。配置结构的 `struct_size` 必须不小于当前已知字段长度，未来追加字段可被旧核心忽略。
- `asternet_client_request_sync` 在调用期间读取请求字段，返回后调用方可以释放所有请求内存。
- `ca_cert_pem` 可传入 PEM 格式 CA bundle；核心会在创建时复制其内容。

## 版本

`asternet_client_config_t.abi_version` 必须设置为 `ASTERNET_ABI_VERSION`。核心校验高 16 位主版本；主版本不匹配返回 `ASTERNET_ERR_ABI_VERSION`，次版本变化保持兼容。

## 生命周期

1. 调用 `asternet_client_create` 创建句柄。
2. 使用 `asternet_client_request_sync` 发起同步请求。
3. 调用 `asternet_client_destroy` 释放句柄；若仍有请求进行，底层 Client 会在最后一个请求结束后延迟释放。

`asternet_client_destroy(nullptr)` 和重复销毁安全返回。销毁会立即使句柄失效，并发请求可以完成后再释放底层 Client；调用方不得在销毁后继续发起新请求。

## 请求与响应

- `host`、`method`、`path` 和有效端口是必填字段。
- headers 使用 `asternet_header_t` 数组传递。
- GET、HEAD、OPTIONS、PUT、DELETE 默认视为幂等；POST 默认非幂等，端侧可显式设置 `idempotent`。
- `protocol_policy` 控制 AUTO、强制协议和 Prefer 策略。
- 成功时 `out_info->body_size` 是完整响应体大小，`body_copied` 是复制到调用方缓冲区的字节数。
- 输出缓冲区为空或容量不足时，请求仍会完成，但返回 `ASTERNET_ERR_BUFFER_TOO_SMALL`。
- `ASTERNET_OK` 表示网络请求和 HTTP 响应解析成功，不等价于 HTTP 状态码为 2xx；实际状态位于 `http_status`。

## 错误与诊断

错误码使用 `asternet_result_t`，端侧不要依赖错误字符串判断逻辑。日志通过 `asternet_set_log_callback` 注册；诊断快照通过 `asternet_client_dump_diagnostics` 获取。

## 当前范围

当前公共 ABI 只包含同步 HTTP 请求、网络变化通知、日志、版本和诊断接口。异步回调、WebSocket、连接池和 HTTPDNS 具体服务适配属于后续版本，不能在端侧提前假设其 ABI。
