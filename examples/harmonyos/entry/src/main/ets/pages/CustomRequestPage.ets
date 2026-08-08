import { router } from '@kit.ArkUI'
import { request, Policy, policyName, protocolName } from '../utils/AsterNetNapi'

@Entry
@Component
struct CustomRequestPage {
  @State host: string = 'www.cloudflare.com'
  @State port: string = '443'
  @State path: string = '/'
  @State methodIndex: number = 0
  @State policyIndex: number = 0
  @State headers: string = ''
  @State body: string = ''
  @State timeout: string = '12000'
  @State isLoading: boolean = false
  @State resultText: string = ''

  private methods: string[] = ['GET', 'POST', 'PUT', 'DELETE']
  private policies: Policy[] = [Policy.AUTO, Policy.HTTP_1_1_ONLY, Policy.HTTP_2_ONLY, Policy.HTTP_3_ONLY]

  build() {
    Column() {
      Row() {
        Button({ type: ButtonType.Circle }) { Text('<').fontSize(18) }
          .width(36).height(36)
          .backgroundColor('#FFFFFF30')
          .onClick(() => router.back())
        Text('Custom Request').fontSize(20).fontWeight(FontWeight.Bold).fontColor(Color.White)
        Blank()
      }
      .width('100%').padding(16).backgroundColor('#4A5C9E')

      Scroll() {
        Column({ space: 12 }) {
          // Target
          Column({ space: 8 }) {
            Text('Target').fontSize(14).fontWeight(FontWeight.Bold).fontColor('#4A5C9E')
            TextInput({ placeholder: 'Host / Domain', text: this.host })
              .onChange(v => this.host = v)
            Row({ space: 8 }) {
              TextInput({ placeholder: 'Port', text: this.port })
                .width(80).type(InputType.Number)
                .onChange(v => this.port = v)
              TextInput({ placeholder: 'Path', text: this.path })
                .layoutWeight(1)
                .onChange(v => this.path = v)
            }
          }
          .width('100%').padding(12).borderRadius(12).backgroundColor('#F5F5F5')

          // Method
          Column({ space: 8 }) {
            Text('Method').fontSize(14).fontWeight(FontWeight.Bold).fontColor('#4A5C9E')
            Row({ space: 8 }) {
              ForEach(this.methods, (m: string, idx: number) => {
                Button(m)
                  .fontSize(13)
                  .backgroundColor(idx === this.methodIndex ? '#4A5C9E' : '#E0E0E0')
                  .fontColor(idx === this.methodIndex ? Color.White : '#333')
                  .borderRadius(16)
                  .onClick(() => this.methodIndex = idx)
              })
            }
          }
          .width('100%').padding(12).borderRadius(12).backgroundColor('#F5F5F5')

          // Protocol
          Column({ space: 8 }) {
            Text('Protocol').fontSize(14).fontWeight(FontWeight.Bold).fontColor('#4A5C9E')
            Row({ space: 8 }) {
              ForEach(this.policies, (p: Policy, idx: number) => {
                Button(policyName(p))
                  .fontSize(12)
                  .backgroundColor(idx === this.policyIndex ? '#4A5C9E' : '#E0E0E0')
                  .fontColor(idx === this.policyIndex ? Color.White : '#333')
                  .borderRadius(16)
                  .onClick(() => this.policyIndex = idx)
              })
            }
            .flexWrap(FlexWrap.Wrap)
          }
          .width('100%').padding(12).borderRadius(12).backgroundColor('#F5F5F5')

          // Headers
          Column({ space: 8 }) {
            Text('Headers (Name: Value, one per line)').fontSize(12).fontColor('#666')
            TextInput({ placeholder: 'content-type: application/json', text: this.headers })
              .height(80)
              .onChange(v => this.headers = v)
          }
          .width('100%').padding(12).borderRadius(12).backgroundColor('#F5F5F5')

          // Body (POST/PUT)
          if (this.methods[this.methodIndex] === 'POST' ||
              this.methods[this.methodIndex] === 'PUT') {
            Column({ space: 8 }) {
              Text('Request Body').fontSize(12).fontColor('#666')
              TextInput({ placeholder: 'Enter request body...', text: this.body })
                .height(100)
                .onChange(v => this.body = v)
            }
            .width('100%').padding(12).borderRadius(12).backgroundColor('#F5F5F5')
          }

          // Settings
          Column({ space: 8 }) {
            Text('Settings').fontSize(14).fontWeight(FontWeight.Bold).fontColor('#4A5C9E')
            Row() {
              Text('Timeout (ms):').fontSize(13)
              TextInput({ text: this.timeout })
                .width(80).type(InputType.Number)
                .onChange(v => this.timeout = v)
            }
          }
          .width('100%').padding(12).borderRadius(12).backgroundColor('#F5F5F5')

          // Send
          Button(this.isLoading ? 'Sending...' : 'Send Request')
            .width('100%').height(48).fontSize(15)
            .backgroundColor(this.isLoading ? '#999' : '#4A5C9E')
            .borderRadius(12)
            .enabled(!this.isLoading && this.host.length > 0)
            .onClick(() => this.send())

          if (this.isLoading) {
            LoadingProgress().width(30).height(30).color('#4A5C9E')
          }

          if (this.resultText) {
            Column({ space: 4 }) {
              Text('Response').fontSize(14).fontWeight(FontWeight.Bold).fontColor('#4A5C9E')
              Text(this.resultText).fontSize(11).fontFamily('monospace')
                .padding(8).backgroundColor('#F5F5F5').borderRadius(8)
            }
            .width('100%').alignItems(HorizontalAlign.Start)
          }
        }
        .padding(16)
      }
    }
    .width('100%').height('100%')
  }

  private send(): void {
    const method = this.methods[this.methodIndex]
    const portNum = parseInt(this.port) || 443
    const timeoutMs = parseInt(this.timeout) || 12000
    const policy = this.policies[this.policyIndex]
    const displayUrl = `https://${this.host}${portNum !== 443 ? ':' + portNum : ''}${this.path}`

    this.isLoading = true
    this.resultText = `Running ${method} ${displayUrl}...`

    setTimeout(() => {
      try {
        const resp = request(this.host, portNum, method, this.path,
          policy, this.headers, this.body, timeoutMs,
          method === 'GET' || method === 'HEAD')

        this.resultText =
          `Request\n${method} ${displayUrl}\nPolicy: ${policyName(policy)}\n\n` +
          `Result\nSuccess: ${resp.result === 0}\n` +
          `Error: ${resp.error} (${resp.result})\n` +
          `HTTP: ${resp.status}  Proto: ${protocolName(resp.protocol)}\n` +
          `DNS: ${resp.dnsMs} ms  Connect: ${resp.connectMs} ms\n` +
          `TLS: ${resp.tlsMs} ms  TTFB: ${resp.ttfbMs} ms\n` +
          `Core: ${resp.totalMs} ms\n` +
          `Body: ${resp.bodySize} B\n\n` +
          resp.body.substring(0, 500)
      } catch (e) {
        this.resultText = `Request failed:\n${JSON.stringify(e)}`
      }
      this.isLoading = false
    }, 100)
  }
}
