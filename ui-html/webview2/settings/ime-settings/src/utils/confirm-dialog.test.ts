import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { confirmDialog } from './confirm-dialog';

class TestElement extends EventTarget {
  textContent = '';
  children: TestElement[] = [];
  parent: TestElement | null = null;
  setAttribute(): void {}
  append(...children: TestElement[]): void {
    children.forEach(child => this.appendChild(child));
  }
  appendChild(child: TestElement): void {
    child.parent = this;
    this.children.push(child);
  }
  remove(): void {
    if (this.parent) this.parent.children = this.parent.children.filter(child => child !== this);
  }
  focus(): void { doc.activeElement = this; }
}

class TestDocument extends EventTarget {
  body = new TestElement();
  activeElement: TestElement | null = null;
  createElement(): TestElement { return new TestElement(); }
}

let doc: TestDocument;
let previousFocus: TestElement;

beforeEach(() => {
  doc = new TestDocument();
  previousFocus = new TestElement();
  previousFocus.focus();
  vi.stubGlobal('document', doc);
});

afterEach(() => vi.unstubAllGlobals());

function findByText(root: TestElement, text: string): TestElement | undefined {
  if (root.textContent === text) return root;
  for (const child of root.children) {
    const found = findByText(child, text);
    if (found) return found;
  }
  return undefined;
}

function buttons(): TestElement[] {
  return [findByText(doc.body, '取消')!, findByText(doc.body, '删除')!];
}

function press(key: string, init: { repeat?: boolean; isComposing?: boolean; shiftKey?: boolean } = {}): Event {
  const event = new Event('keydown', { cancelable: true });
  Object.defineProperty(event, 'key', { value: key });
  Object.defineProperty(event, 'repeat', { value: init.repeat ?? false });
  Object.defineProperty(event, 'isComposing', { value: init.isComposing ?? false });
  Object.defineProperty(event, 'shiftKey', { value: init.shiftKey ?? false });
  doc.dispatchEvent(event);
  return event;
}

async function settled(result: Promise<boolean>): Promise<boolean | 'pending'> {
  return Promise.race([result, new Promise<'pending'>(resolve => setTimeout(() => resolve('pending'), 0))]);
}

it('cancels when Enter is pressed with Cancel focused', async () => {
  const result = confirmDialog('确定删除测试词条吗？');
  buttons()[0]!.focus();
  expect(press('Enter').defaultPrevented).toBe(true);
  expect(await result).toBe(false);
  expect(doc.body.children).toHaveLength(0);
  expect(doc.activeElement).toBe(previousFocus);
});

it('confirms when Enter is pressed with Confirm focused', async () => {
  const result = confirmDialog('确定删除测试词条吗？');
  expect(doc.activeElement).toBe(buttons()[1]);
  expect(press('Enter').defaultPrevented).toBe(true);
  expect(await result).toBe(true);
});

it('cancels on Escape even with Confirm focused', async () => {
  const result = confirmDialog('确定删除测试词条吗？');
  expect(press('Escape').defaultPrevented).toBe(true);
  expect(await result).toBe(false);
});

it.each([
  ['the page behind the dialog', () => previousFocus],
  ['nothing', () => null],
])('does nothing on Enter when focus is on %s', async (_label, target) => {
  const result = confirmDialog('确定删除测试词条吗？');
  doc.activeElement = target();
  expect(press('Enter').defaultPrevented).toBe(true);
  expect(await settled(result)).toBe('pending');
  buttons()[0]!.dispatchEvent(new Event('click'));
  expect(await result).toBe(false);
});

it('ignores an auto-repeated Enter from the key that opened the dialog', async () => {
  const result = confirmDialog('确定删除测试词条吗？');
  expect(press('Enter', { repeat: true }).defaultPrevented).toBe(true);
  expect(await settled(result)).toBe('pending');
  press('Enter');
  expect(await result).toBe(true);
});

it('leaves an IME composition Enter alone', async () => {
  const result = confirmDialog('确定删除测试词条吗？');
  expect(press('Enter', { isComposing: true }).defaultPrevented).toBe(false);
  expect(await settled(result)).toBe('pending');
  press('Escape');
  expect(await result).toBe(false);
});

it.each([false, true])('keeps Tab focus on the dialog buttons (shift: %s)', async (shiftKey) => {
  const result = confirmDialog('确定删除测试词条吗？');
  const [cancel, confirm] = buttons();
  expect(press('Tab', { shiftKey }).defaultPrevented).toBe(true);
  expect(doc.activeElement).toBe(cancel);
  press('Tab', { shiftKey });
  expect(doc.activeElement).toBe(confirm);
  doc.activeElement = previousFocus;
  press('Tab', { shiftKey });
  expect(doc.activeElement).toBe(confirm);
  press('Escape');
  await result;
});

it.each([[0, false], [1, true]] as const)('keeps button %i click behavior', async (index, expected) => {
  const result = confirmDialog('确定删除测试词条吗？');
  buttons()[index]!.dispatchEvent(new Event('click'));
  expect(await result).toBe(expected);
});
