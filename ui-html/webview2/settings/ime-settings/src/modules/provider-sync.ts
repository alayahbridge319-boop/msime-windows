import { onHostMessage } from '../utils/host-messages';
import { serializeHostMessage } from '../../../../shared/messages';

// One entry of the provider registry: any OpenAI-compatible service the user added. The whole
// list is synced in one message, which keeps the store and the UI convergent without CRUD
// bookkeeping.
export type RegistryEntry = {
  id: string;
  name: string;
  endpoint: string;
  token: string;
  model: string;
};

let nextRequestId = 0;

export function sendProviderSync(entries: RegistryEntry[], status: HTMLElement | null): Promise<boolean> {
  return new Promise(resolve => {
    const webview = window.chrome?.webview;
    if (!webview) {
      resolve(false);
      return;
    }
    const requestId = `${Date.now()}-${++nextRequestId}`;
    const unsubscribe = onHostMessage('aiProviderSyncResult', payload => {
      if (payload.requestId !== requestId) return;
      cleanup();
      if (status) {
        status.textContent = payload.message;
        status.dataset.kind = payload.ok ? 'success' : 'error';
      }
      resolve(payload.ok);
    });
    // A reply that never comes (server restart mid-save) must not leave the button dead.
    const timer = window.setTimeout(() => {
      cleanup();
      if (status) {
        status.textContent = '保存超时，请重试。';
        status.dataset.kind = 'error';
      }
      resolve(false);
    }, 10000);
    const cleanup = (): void => {
      window.clearTimeout(timer);
      unsubscribe();
    };
    webview.postMessage(serializeHostMessage({
      type: 'aiProviderSync',
      data: { requestId, entries }
    }));
  });
}

// Mirrors the server's slug rule closely enough for the UI: lowercase ASCII, separators become
// dashes, collisions get a numeric suffix. The server keeps the authoritative version.
export function makeProviderId(name: string, existing: RegistryEntry[]): string {
  const slug = (name.trim().toLowerCase().replace(/[^a-z0-9_-]+/g, '-').replace(/^-+|-+$/g, '') ||
    'provider');
  const taken = new Set(existing.map(entry => entry.id));
  if (!taken.has(slug)) return slug;
  for (let suffix = 2; ; suffix++) {
    const candidate = `${slug}-${suffix}`;
    if (!taken.has(candidate)) return candidate;
  }
}