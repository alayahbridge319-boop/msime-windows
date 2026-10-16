import { serializeHostMessage } from '../../../../shared/messages';
import { onHostMessage } from '../utils/host-messages';

export type CredentialTestService =
  | 'translation.tencent'
  | 'translation.niutrans'
  | 'translation.custom'
  | 'translation.ai'
  | 'voice.asr'
  | 'voice.polish'
  | 'ai.assistant';

type TestConfig = Record<string, string>;
let nextRequestId = 0;

// Server 单次测试最多连 5 秒、请求 15 秒，留出余量后仍没回复就当作丢了，免得按钮一直卡在测试中。
export const CREDENTIAL_TEST_TIMEOUT_MS = 30000;

function sameConfig(a: TestConfig, b: TestConfig): boolean {
  const keys = Object.keys(a);
  return keys.length === Object.keys(b).length && keys.every(key => a[key] === b[key]);
}

// readSnapshot 用来判断结果是否过时，默认就是发出去的 readConfig。
// 发给 Server 的配置不完整时（比如候选词翻译走 AI 服务时 Server 自己读 AI 辅助配置），
// 调用方要另给一份能反映实际凭据的快照，否则在途期间改了凭据也看不出来。
export function setupCredentialTest(
  buttonId: string,
  statusId: string,
  service: () => CredentialTestService,
  readConfig: () => TestConfig,
  readSnapshot: () => TestConfig = readConfig
): void {
  const button = document.getElementById(buttonId) as HTMLButtonElement | null;
  const status = document.getElementById(statusId);
  if (!button || !status) return;

  let pending: {
    requestId: string; service: CredentialTestService; snapshot: TestConfig; timer: number;
  } | null = null;
  const idleLabel = button.textContent?.trim() || '测试配置';
  const finish = (): void => {
    if (pending) window.clearTimeout(pending.timer);
    pending = null;
    button.disabled = false;
    button.textContent = idleLabel;
  };
  onHostMessage('apiCredentialTestResult', payload => {
    if (!pending || payload.requestId !== pending.requestId) return;
    const configMatches = pending.service === service() && sameConfig(pending.snapshot, readSnapshot());
    finish();
    if (!configMatches) {
      status.textContent = '配置已变更，请重新测试';
      delete status.dataset.kind;
      return;
    }
    status.textContent = payload.message;
    status.dataset.kind = payload.ok ? 'success' : 'error';
  });

  button.addEventListener('click', () => {
    if (!window.chrome?.webview || pending) return;
    const requestId = `${Date.now()}-${++nextRequestId}`;
    const currentService = service();
    const config = { ...readConfig() };
    pending = {
      requestId,
      service: currentService,
      snapshot: { ...readSnapshot() },
      timer: window.setTimeout(() => {
        if (pending?.requestId !== requestId) return;
        finish();
        status.textContent = '测试超时，请重试';
        status.dataset.kind = 'error';
      }, CREDENTIAL_TEST_TIMEOUT_MS)
    };
    button.disabled = true;
    button.textContent = '测试中…';
    status.textContent = '正在连接服务，请稍候…';
    status.dataset.kind = 'pending';
    window.chrome.webview.postMessage(serializeHostMessage({
      type: 'apiCredentialTest',
      data: { requestId, service: currentService, config }
    }));
  });
}
