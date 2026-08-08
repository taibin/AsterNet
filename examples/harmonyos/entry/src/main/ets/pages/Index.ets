import { router } from '@kit.ArkUI'
import { request, Policy, policyName, protocolName } from '../utils/AsterNetNapi'
import { promptAction } from '@kit.ArkUI'

// ---- 预设场景 ----
interface Scenario {
  name: string
  description: string
  host: string
  port: number
  path: string
  method: string
  policy: Policy
  headers: string
  body: string
}

const SCENARIOS: Scenario[] = [
  {
    name: 'Automatic GET',
    description: 'HTTPS GET. Tries HTTP/3, then H2, then H1.1.',
    host: 'www.cloudflare.com', port: 443, path: '/', method: 'GET',
    policy: Policy.AUTO, headers: '', body: ''
  },
  {
    name: 'HTTP/1.1 GET',
    description: 'Forces HTTP/1.1 over TLS.',
    host: 'www.cloudflare.com', port: 443, path: '/', method: 'GET',
    policy: Policy.HTTP_1_1_ONLY, headers: '', body: ''
  },
  {
    name: 'HTTP/2 GET',
    description: 'Forces HTTP/2 over TLS.',
    host: 'www.cloudflare.com', port: 443, path: '/', method: 'GET',
    policy: Policy.HTTP_2_ONLY, headers: '', body: ''
  },
  {
    name: 'HTTP/3 GET',
    description: 'Forces HTTP/3 over QUIC.',
    host: 'www.cloudflare.com', port: 443, path: '/', method: 'GET',
    policy: Policy.HTTP_3_ONLY, headers: '', body: ''
  },
  {
    name: 'HTTP/2 POST',
    description: 'JSON POST (non-idempotent).',
    host: 'nghttp2.org', port: 443, path: '/httpbin/post', method: 'POST',
    policy: Policy.HTTP_2_ONLY,
    headers: 'content-type: application/json',
    body: '{"source":"AsterNet HarmonyOS Demo"}'
  },
]

@Entry
@Component
struct Index {
  @State selectedIndex: number = 0
  @State isLoading: boolean = false
  @State resultText: string = ''
  @State nativeVersion: string = ''

  aboutToAppear(): void {
    // 版本号在后续 SDK 就绪后可获取
  }

  build() {
    Column() {
      // 标题栏
      Row() {
        Text('AsterNet Presets')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .fontColor(Color.White)
        Blank()
        Button('Custom')
          .fontSize(14)
          .backgroundColor('#FFFFFF30')
          .onClick(() => router.pushUrl({ url: 'pages/CustomRequestPage' }))
      }
      .width('100%')
      .padding(16)
      .backgroundColor('#4A5C9E')

      Scroll() {
        Column({ space: 12 }) {
          // 场景选择
          Column() {
            Text('Scenario').fontSize(14).fontColor('#666')
            Row({ space: 8 }) {
              ForEach(SCENARIOS, (s: Scenario, idx: number) => {
                Button(s.name)
                  .fontSize(12)
                  .backgroundColor(idx === this.selectedIndex ? '#4A5C9E' : '#E0E0E0')
                  .fontColor(idx === this.selectedIndex ? Color.White : '#333')
                  .borderRadius(16)
                  .onClick(() => this.selectedIndex = idx)
              })
            }
            .width('100%')
            .flexWrap(FlexWrap.Wrap)
          }
          .width('100%')
          .padding(16)
          .borderRadius(12)
          .backgroundColor('#F5F5F5')

          // 场景详情
          Column({ space: 4 }) {
            Text(SCENARIOS[this.selectedIndex].description)
              .fontSize(13).fontColor('#666')
            Text(`${SCENARIOS[this.selectedIndex].method} https://${SCENARIOS[this.selectedIndex].host}${SCENARIOS[this.selectedIndex].path}`)
              .fontSize(11).fontColor('#999').fontFamily('monospace')
            Text(`Policy: ${policyName(SCENARIOS[this.selectedIndex].policy)}`)
              .fontSize(11).fontColor('#999')
          }
          .width('100%')
          .padding(12)
          .borderRadius(12)
          .backgroundColor('#F5F5F5')
          .alignItems(HorizontalAlign.Start)

          // 发送按钮
          Button(this.isLoading ? 'Sending...' : 'Send Request')
            .width('100%')
            .height(48)
            .fontSize(15)
            .backgroundColor(this.isLoading ? '#999' : '#4A5C9E')
            .borderRadius(12)
            .enabled(!this.isLoading)
            .onClick(() => this.send())

          if (this.isLoading) {
            LoadingProgress()
              .width(30).height(30)
              .color('#4A5C9E')
          }

          // 结果
          if (this.resultText) {
            Column({ space: 4 }) {
              Text('Response').fontSize(14).fontWeight(FontWeight.Bold).fontColor('#4A5C9E')
              Text(this.resultText)
                .fontSize(11)
                .fontFamily('monospace')
                .padding(8)
                .backgroundColor('#F5F5F5')
                .borderRadius(8)
            }
            .width('100%')
            .alignItems(HorizontalAlign.Start)
          }
        }
        .padding(16)
      }
    }
    .width('100%')
    .height('100%')
  }

  private send(): void {
    const s = SCENARIOS[this.selectedIndex]
    this.isLoading = true
    this.resultText = `Running ${s.name}...`

    setTimeout(() => {
      try {
        const resp = request(s.host, s.port, s.method, s.path,
          s.policy, s.headers, s.body, 12000,
          s.method === 'GET' || s.method === 'HEAD')

        this.resultText =
          `Request\n${s.name}\n${s.method} ${s.host}${s.path}\n` +
          `Policy: ${policyName(s.policy)}\n\n` +
          `Result\nSuccess: ${resp.result === 0}\n` +
          `Error: ${resp.error} (${resp.result})\n` +
          `HTTP: ${resp.status}  Proto: ${protocolName(resp.protocol)}\n` +
          `Body: ${resp.bodySize} B  Core: ${resp.totalMs} ms\n\n` +
          resp.body.substring(0, 400)
      } catch (e) {
        this.resultText = `Request failed:\n${JSON.stringify(e)}`
      }
      this.isLoading = false
    }, 100)
  }
}
