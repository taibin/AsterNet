import { UIAbility, Want, AbilityConstant } from '@kit.AbilityKit'
import { window } from '@kit.ArkUI'
import { createClient, destroy, version } from '../utils/AsterNetNapi'
import { hilog } from '@kit.PerformanceAnalysisKit'

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, 'AsterNet', `AsterNet ${version()} starting`)
    createClient(true)
  }

  onDestroy(): void {
    destroy()
    hilog.info(0x0000, 'AsterNet', 'Client destroyed')
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(0x0000, 'AsterNet', `Failed to load: ${err.message}`)
      }
    })
  }
}
