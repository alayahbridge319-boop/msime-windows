import { afterEach, beforeEach, expect, it, vi } from 'vitest';

vi.mock('../utils/dictionary-file', () => ({ readDictionaryFile: vi.fn() }));
vi.mock('../utils/dictionary-pagination', () => ({ createDictionaryPager: vi.fn(), DICTIONARY_PAGE_SIZE: 50 }));
vi.mock('../utils/host-messages', () => ({ onHostMessage: vi.fn() }));
vi.mock('../utils/overlay-host', () => ({ hoistOverlay: vi.fn() }));
vi.mock('../utils/confirm-dialog', () => ({ confirmDialog: vi.fn() }));

import { readDictionaryFile } from '../utils/dictionary-file';
import { onHostMessage } from '../utils/host-messages';
import { confirmDialog } from '../utils/confirm-dialog';

class TestElement extends EventTarget {
  value = '';
  textContent = '';
  innerHTML = '';
  placeholder = '';
  className = '';
  dataset: Record<string, string> = {};
  style: Record<string, string> = {};
  files = [{} as File];
  children: TestElement[] = [];
  classes = new Set<string>();
  classList = {
    add: (name: string) => { this.classes.add(name); },
    remove: (name: string) => { this.classes.delete(name); },
    contains: (name: string) => this.classes.has(name),
  };
  setAttribute(): void {}
  appendChild(child: TestElement): void { this.children.push(child); }
  append(...children: TestElement[]): void { this.children.push(...children); }
  replaceChildren(...children: TestElement[]): void { this.children = children; }
}

let elements: Map<string, TestElement>;
let tabs: TestElement[];
let postMessage: ReturnType<typeof vi.fn>;

beforeEach(async () => {
  vi.resetModules();
  vi.clearAllMocks();
  elements = new Map();
  for (const id of [
    'dictSearch', 'dictSearchButton', 'dictHint', 'dictTableHeader', 'dictRows', 'dictImportFile',
    'dictImportHansFile', 'dictModal', 'dictToast', 'dictToastMessage', 'dictToastIcon',
  ]) {
    elements.set(id, new TestElement());
  }
  tabs = ['quanpin', 'wubi', 'english'].map(dictionary => {
    const tab = new TestElement();
    tab.dataset.dictionary = dictionary;
    return tab;
  });
  tabs[0]!.classList.add('active');
  postMessage = vi.fn();
  vi.stubGlobal('document', {
    getElementById: (id: string) => elements.get(id) ?? null,
    createElement: () => new TestElement(),
    querySelectorAll: (selector: string) => selector === '.dict-tab' ? tabs : [],
    querySelector: (selector: string) => selector === '.dict-tab.active'
      ? tabs.find(tab => tab.classList.contains('active')) : null,
    addEventListener: vi.fn(),
  });
  vi.stubGlobal('window', {
    chrome: { webview: { postMessage } },
    requestAnimationFrame: vi.fn(),
    addEventListener: vi.fn(),
    setTimeout: vi.fn(() => 1),
    clearTimeout: vi.fn(),
  });
  vi.stubGlobal('MsimeProtocol', {
    version: 1,
    validate: () => true,
    serializeClientMessage: (message: unknown) => JSON.stringify(message),
  });
  const { setupDictionary } = await import('./dict');
  setupDictionary();
});

afterEach(() => vi.unstubAllGlobals());

function requests(): Array<{ action: string; dictionary: string; requestId: string } & Record<string, unknown>> {
  return postMessage.mock.calls.map(([message]) => JSON.parse(message as string).data);
}

function respond(payload: Record<string, unknown>): void {
  const handler = vi.mocked(onHostMessage).mock.calls.find(([type]) => type === 'dictionaryResponse')![1];
  handler({ type: 'dictionaryResponse', ok: true, rows: [], ...payload } as never);
}

function search(text: string): ReturnType<typeof requests>[number] {
  elements.get('dictSearch')!.value = text;
  elements.get('dictSearchButton')!.dispatchEvent(new Event('click'));
  return requests().at(-1)!;
}

function findButton(root: TestElement, text: string): TestElement | undefined {
  if (root.textContent === text) return root;
  for (const child of root.children) {
    const found = findButton(child, text);
    if (found) return found;
  }
  return undefined;
}

it.each([
  ['dictImportFile', 'import', '测试\tceshi'],
  ['dictImportHansFile', 'importHans', '测试'],
])('keeps the selected dictionary while %s is being read', async (inputId, action, content) => {
  let finishRead!: (content: string) => void;
  vi.mocked(readDictionaryFile).mockReturnValueOnce(new Promise(resolve => { finishRead = resolve; }));
  elements.get(inputId)!.dispatchEvent(new Event('change'));
  expect(postMessage).not.toHaveBeenCalled();
  tabs[1]!.dispatchEvent(new Event('click'));
  finishRead(content);
  await vi.waitFor(() => expect(postMessage).toHaveBeenCalled());

  const request = requests()[0]!;
  expect(request).toMatchObject({ action, dictionary: 'quanpin', content });

  // An import completed on another tab must not close the current tab's editor.
  elements.get('dictModal')!.classList.add('open');
  respond({ requestId: request.requestId, message: '导入成功' });
  expect(elements.get('dictModal')!.classList.contains('open')).toBe(true);
  expect(elements.get('dictToastMessage')!.textContent).toBe('全拼词库：导入成功');
});

it('imports into the active dictionary when the tab does not change', async () => {
  tabs[1]!.dispatchEvent(new Event('click'));
  vi.mocked(readDictionaryFile).mockResolvedValueOnce('测试\tiya');
  elements.get('dictImportFile')!.dispatchEvent(new Event('change'));
  await vi.waitFor(() => expect(postMessage).toHaveBeenCalled());
  expect(requests()[0]).toMatchObject({ action: 'import', dictionary: 'wubi', content: '测试\tiya' });
});

it('deletes from the dictionary the row came from even if the tab changes during confirmation', async () => {
  const query = search('nihao');
  respond({ requestId: query.requestId, rows: [{ code: 'nihao', word: '你好', weight: 10 }] });
  let answer!: (confirmed: boolean) => void;
  vi.mocked(confirmDialog).mockReturnValueOnce(new Promise(resolve => { answer = resolve; }));
  findButton(elements.get('dictRows')!, '删除')!.dispatchEvent(new Event('click'));
  tabs[2]!.dispatchEvent(new Event('click'));
  answer(true);
  await vi.waitFor(() => expect(requests().at(-1)!.action).toBe('delete'));
  expect(requests().at(-1)).toMatchObject({ dictionary: 'quanpin', oldCode: 'nihao', oldWord: '你好' });
});

it('does not reuse another tab\'s query to refresh after an import', async () => {
  search('nihao');
  tabs[1]!.dispatchEvent(new Event('click'));
  vi.mocked(readDictionaryFile).mockResolvedValueOnce('测试\tiya');
  elements.get('dictImportFile')!.dispatchEvent(new Event('change'));
  await vi.waitFor(() => expect(requests().at(-1)!.action).toBe('import'));
  const sent = postMessage.mock.calls.length;
  respond({ requestId: requests().at(-1)!.requestId, message: '导入成功 1 条' });
  expect(postMessage).toHaveBeenCalledTimes(sent);
  expect(elements.get('dictToastMessage')!.textContent).toBe('导入成功 1 条');
  expect(elements.get('dictToast')!.className).toContain('success');
});

it('refreshes the current query with its original text after an import', async () => {
  search('nihao');
  elements.get('dictSearch')!.value = 'typed-but-not-searched';
  vi.mocked(readDictionaryFile).mockResolvedValueOnce('测试\tceshi');
  elements.get('dictImportFile')!.dispatchEvent(new Event('change'));
  await vi.waitFor(() => expect(requests().at(-1)!.action).toBe('import'));
  respond({ requestId: requests().at(-1)!.requestId, message: '导入成功' });
  expect(requests().at(-1)).toMatchObject({ action: 'query', dictionary: 'quanpin', code: 'nihao' });
});
