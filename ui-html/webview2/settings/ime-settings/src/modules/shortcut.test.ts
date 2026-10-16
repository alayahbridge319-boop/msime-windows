import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyShortcutConfig, setupShortcut } from './shortcut';
import { updateConfig } from './config-sync';
import { readFileSync } from 'node:fs';
import type { ShortcutMetadata } from './shortcut-recorder';
const fixture: ShortcutMetadata = JSON.parse(readFileSync(new URL('../../../../../../engine/contracts/tests/input_state_shortcuts.json', import.meta.url), 'utf8'));
let snapshot: ShortcutMetadata;
function applyBinding(binding: string, index = 2): void {
  snapshot.actions[index].binding = binding;
  applyShortcutConfig({ input_state_shortcuts: structuredClone(snapshot) });
}

vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));

class Checkbox extends EventTarget {
  checked = true;
  textContent = '';
  hidden = true;
  focus = vi.fn();
  private attributes = new Map<string, string>();
  getAttribute(name: string): string | null { return this.attributes.get(name) ?? null; }
  setAttribute(name: string, value: string): void { this.attributes.set(name, value); }
}

let characterSet: Checkbox;
let trilingualCycle: Checkbox;
let editor: Record<string, Checkbox>;
beforeEach(() => {
  vi.clearAllMocks();
  characterSet = new Checkbox();
  trilingualCycle = new Checkbox();
  trilingualCycle.checked = false;
  snapshot = structuredClone(fixture);
  editor = Object.fromEntries(snapshot.actions.flatMap(action =>
    ['Button', 'Clear', 'Reset', 'Status', 'Confirmation', 'Confirm', 'Cancel'].map(suffix => [action.editor + suffix, new Checkbox()])));
  vi.stubGlobal('document', {
    getElementById: (id: string) => {
      if (id === 'characterSetShortcutCheckbox') return characterSet;
      if (id === 'trilingualCycleCheckbox') return trilingualCycle;
      return editor[id] ?? null;
    },
    querySelectorAll: () => [],
  });
  setupShortcut();
  applyShortcutConfig({ input_state_shortcuts: structuredClone(snapshot) });
});
afterEach(() => vi.unstubAllGlobals());

it('applies the persisted shortcut setting without writing it back', () => {
  applyShortcutConfig({ toggle_character_set_ctrl_shift_f: false });
  expect(characterSet.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
  applyShortcutConfig({ toggle_character_set_ctrl_shift_f: true });
  expect(characterSet.checked).toBe(true);
});

it('writes only the character-set shortcut setting when the user disables it', () => {
  characterSet.checked = false;
  characterSet.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_character_set_ctrl_shift_f', false);
});

it('applies the persisted trilingual cycle switch without writing it back', () => {
  applyShortcutConfig({ trilingual_cycle: true });
  expect(trilingualCycle.checked).toBe(true);
  applyShortcutConfig({ trilingual_cycle: false });
  expect(trilingualCycle.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
});

it('writes only the trilingual cycle switch when the user toggles it', () => {
  trilingualCycle.checked = true;
  trilingualCycle.dispatchEvent(new Event('change'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.trilingual_cycle', true);
});

it('accepts older configuration snapshots with no shortcut field', () => {
  applyShortcutConfig(undefined);
  applyShortcutConfig({ switch_language_shift: false });
  expect(characterSet.checked).toBe(true);
  expect(trilingualCycle.checked).toBe(false);
  expect(updateConfig).not.toHaveBeenCalled();
});

function key(code: string, options: Record<string, unknown> = {}): Event {
  const keyCode = /^Key[A-Z]$/.test(code) ? code.charCodeAt(3) : /^Digit[0-9]$/.test(code) ? code.charCodeAt(5) :
    /^F\d+$/.test(code) ? 111 + Number(code.slice(1)) : ({ Period: 190, ControlLeft: 17, AltLeft: 18,
      AltRight: 18, Escape: 27, Tab: 9, Space: 32 } as Record<string, number>)[code] ?? 0;
  const event = Object.assign(new Event('keydown', { cancelable: true }), {
    code, keyCode, location: 0, ctrlKey: true, altKey: false, shiftKey: false, metaKey: false, repeat: false,
    getModifierState: () => false, ...options,
  });
  editor.punctuationHotkeyButton.dispatchEvent(event);
  return event;
}

function confirmBinding(): void {
  editor.punctuationHotkeyConfirm.dispatchEvent(new Event('click'));
}

it('applies default, custom and explicitly disabled bindings without writing back', () => {
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  applyBinding('Ctrl+KeyA');
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + A');
  applyBinding('');
  expect(editor.punctuationHotkeyButton.textContent).toBe('未绑定');
  expect(updateConfig).not.toHaveBeenCalled();
});

it('re-enables the editors when the host never answers a save', () => {
  vi.useFakeTimers();
  try {
    editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
    key('Period', { shiftKey: true });
    expect((editor.fullWidthHotkeyButton as unknown as HTMLButtonElement).disabled).toBe(true);
    vi.advanceTimersByTime(5000);
    expect((editor.fullWidthHotkeyButton as unknown as HTMLButtonElement).disabled).toBe(false);
    expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
    expect(editor.punctuationHotkeyStatus.textContent).toContain('请重试');
  } finally {
    vi.useRealTimers();
  }
});

it('saves a chord with no known conflict once and waits for the persisted host snapshot', () => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('Period', { shiftKey: true });
  key('Period', { shiftKey: true, repeat: true });
  expect(editor.punctuationHotkeyStatus.textContent).toBe('正在保存…');
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  editor.punctuationHotkeyButton.dispatchEvent(new Event('blur'));
  confirmBinding();
  confirmBinding();
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+Shift+Period');
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  applyBinding('Ctrl+Shift+Period');
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + Shift + .');
});

it('records Win32 keys with no scan code and cancels them with Escape', () => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('', { keyCode: 17 });
  key('', { keyCode: 16, shiftKey: true });
  expect(editor.punctuationHotkeyStatus.textContent).toContain('Esc');
  key('', { keyCode: 190, shiftKey: true });
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+Shift+Period');
  applyBinding('Ctrl+Shift+Period');
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  expect(key('', { keyCode: 9 }).defaultPrevented).toBe(false);
  key('', { keyCode: 27 });
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + Shift + .');
  expect(editor.punctuationHotkeyButton.getAttribute('aria-pressed')).toBe('false');
  expect(updateConfig).toHaveBeenCalledTimes(1);
});

it('uses virtual keys with or without scan codes and rejects numpad aliases', () => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('', { keyCode: 70, shiftKey: true });
  key('', { keyCode: 190, altKey: true });
  key('NumpadDecimal', { keyCode: 190 });
  expect(updateConfig).not.toHaveBeenCalled();
  key('', { keyCode: 71 });
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+KeyG');
});

it.each(['punctuationHotkeyClear', 'punctuationHotkeyReset'])('writes the %s action', id => {
  editor[id].dispatchEvent(new Event('click'));
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation',
    id === 'punctuationHotkeyClear' ? '' : 'Ctrl+Period');
});

it('preserves the confirmed value if saving fails and the host resends it', () => {
  editor.punctuationHotkeyClear.dispatchEvent(new Event('click'));
  applyBinding('Ctrl+Period');
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  expect(editor.punctuationHotkeyStatus.textContent).toBe('');
});

it('cancels on Escape or blur, leaves Tab navigation alone and ignores keys outside recording', () => {
  expect(key('Period').defaultPrevented).toBe(false);
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('Escape');
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  expect(key('Tab').defaultPrevented).toBe(false);
  editor.punctuationHotkeyButton.dispatchEvent(new Event('blur'));
  key('Period');
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
});

it('rejects reserved, bare, Win and AltGr chords and ignores modifiers and repeats', () => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('ControlLeft');
  key('Period', { repeat: true });
  key('KeyF', { shiftKey: true });
  key('KeyE', { shiftKey: true });
  key('F9');
  key('F9', { shiftKey: true });
  key('AltLeft', { ctrlKey: false, altKey: true });
  key('F4', { ctrlKey: false, altKey: true });
  key('F4', { ctrlKey: false, altKey: true, shiftKey: true });
  key('Period', { ctrlKey: false });
  key('Period', { metaKey: true });
  key('Period', { altKey: true });
  key('Period', { getModifierState: () => true });
  expect(updateConfig).not.toHaveBeenCalled();
  key('Digit2');
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+Digit2');
});

it.each([
  ['KeyQ', 65, 'KeyA'], // AZERTY: physical Q produces VK_A.
  ['KeyY', 90, 'KeyZ'], // QWERTZ: physical Y produces VK_Z.
  ['Semicolon', 188, 'Comma'], // OEM keys also follow the Windows VK.
])('records %s using its Windows virtual key', (code, keyCode, expected) => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key(code, { keyCode });
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', `Ctrl+${expected}`);
});

it.each([
  ['KeyE', { shiftKey: true }, '英文候选'],
  ['KeyF', { shiftKey: true }, '简繁'],
  ['F9', {}, '语音'],
  ['F9', { shiftKey: true }, '语音'],
])('names the feature that owns %s %j', (code, options, feature) => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('AltLeft', { ctrlKey: false, altKey: true });
  key(code, options);
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyStatus.textContent).toContain(feature);
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  confirmBinding();
  expect(updateConfig).not.toHaveBeenCalled();
});

it.each([false, true])('rejects Alt chords with Shift=%s for all three actions', shiftKey => {
  for (const action of snapshot.actions) {
    const button = editor[action.editor + 'Button'];
    button.dispatchEvent(new Event('click'));
    for (const code of ['AltLeft', 'AltRight', '']) {
      button.dispatchEvent(Object.assign(new Event('keydown', { cancelable: true }), {
        code, keyCode: 18, location: code === 'AltRight' ? 2 : 1,
        ctrlKey: false, altKey: true, shiftKey, getModifierState: () => false,
      }));
      button.dispatchEvent(Object.assign(new Event('keydown', { cancelable: true }), {
        code: 'KeyJ', keyCode: 74, ctrlKey: false, altKey: true, shiftKey,
        getModifierState: () => false,
      }));
      expect(updateConfig).not.toHaveBeenCalled();
      expect(editor[action.editor + 'Status'].textContent).toContain('请选择 Ctrl');
      expect(button.getAttribute('aria-pressed')).toBe('true');
    }
  }
});

it('accepts Ctrl after rejecting an Alt chord in the same recording', () => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyJ', { ctrlKey: false, altKey: true });
  expect(updateConfig).not.toHaveBeenCalled();
  key('KeyJ', { shiftKey: true });
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+Shift+KeyJ');
});

it.each([
  ['KeyC', false, '复制'], ['KeyV', false, '粘贴'], ['KeyX', false, '剪切'],
  ['KeyA', false, '全选'], ['KeyZ', false, '撤销'], ['KeyY', false, '重做'],
  ['KeyS', false, '保存'], ['KeyF', false, '查找'], ['KeyH', false, '替换'],
  ['KeyP', false, '打印'], ['KeyO', false, '打开'], ['KeyN', false, '新建'],
  ['KeyW', false, '关闭'], ['F4', false, '关闭'], ['KeyT', false, '新建标签页'],
  ['KeyV', true, '粘贴为纯文本'], ['KeyZ', true, '重做'],
  ['KeyS', true, '另存为'], ['KeyT', true, '重新打开'],
] as const)('warns about %s Shift=%s (%s) before saving', (code, shiftKey, action) => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key(code, { shiftKey });
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  expect(editor.punctuationHotkeyStatus.textContent).toContain(action);
  expect(editor.punctuationHotkeyStatus.textContent).toContain('冲突');
  expect(editor.punctuationHotkeyStatus.textContent).toContain('仍要绑定吗？');
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(false);
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation',
    `Ctrl+${shiftKey ? 'Shift+' : ''}${code}`);
});

it.each(['cancel', 'escape', 'confirmation-escape'])('keeps the original binding after %s', action => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyC');
  if (action === 'cancel') editor.punctuationHotkeyCancel.dispatchEvent(new Event('click'));
  else if (action === 'escape') key('Escape');
  else editor.punctuationHotkeyConfirmation.dispatchEvent(Object.assign(new Event('keydown'), { key: 'Escape' }));
  confirmBinding();
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyButton.textContent).toBe('Ctrl + .');
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  expect(editor.punctuationHotkeyStatus.textContent).toBe('');
});

it('replaces a pending chord when recording starts again', () => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyC');
  key('KeyV'); // Keys outside recording cannot silently replace the pending chord.
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  confirmBinding();
  expect(updateConfig).not.toHaveBeenCalled();
  key('KeyG', { shiftKey: true });
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  confirmBinding();
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation', 'Ctrl+Shift+KeyG');
});

it.each(['punctuationHotkeyClear', 'punctuationHotkeyReset'])('discards a pending chord on %s', id => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyC');
  editor[id].dispatchEvent(new Event('click'));
  confirmBinding();
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith('keybindings.toggle_punctuation',
    id === 'punctuationHotkeyClear' ? '' : 'Ctrl+Period');
});

function keyFor(index: number, code: string, options: Record<string, unknown> = {}): void {
  const button = editor[snapshot.actions[index].editor + 'Button'];
  const vk = code === 'Space' ? 32 : code === 'Period' ? 190 : code.charCodeAt(3);
  button.dispatchEvent(Object.assign(new Event('keydown', { cancelable: true }), {
    code, keyCode: vk, location: 0, ctrlKey: true, altKey: false, shiftKey: false,
    metaKey: false, repeat: false, getModifierState: () => false, ...options,
  }));
}
it.each([0, 1, 2])('uses the same recorder and persisted snapshot for action %s', index => {
  const action = snapshot.actions[index];
  editor[action.editor + 'Button'].dispatchEvent(new Event('click'));
  keyFor(index, 'KeyG', { shiftKey: true });
  expect(updateConfig).toHaveBeenCalledExactlyOnceWith(`keybindings.${action.key}`, 'Ctrl+Shift+KeyG');
  expect(editor[action.editor + 'Status'].textContent).toBe('正在保存…');
  applyBinding('Ctrl+Shift+KeyG', index);
  expect(editor[action.editor + 'Button'].textContent).toBe('Ctrl + Shift + G');
});
it('releases a cleared default and checks reset against its new owner', () => {
  applyBinding('', 0);
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyE', { shiftKey: true });
  expect(updateConfig).toHaveBeenLastCalledWith('keybindings.toggle_punctuation', 'Ctrl+Shift+KeyE');
  applyBinding('Ctrl+Shift+KeyE');
  vi.clearAllMocks();
  editor.englishCandidatesHotkeyReset.dispatchEvent(new Event('click'));
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.englishCandidatesHotkeyStatus.textContent).toContain('中英文标点');
});
it('records Space and distinguishes its dynamic owner from Windows warnings', () => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('Space', { shiftKey: true });
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyStatus.textContent).toContain('全角');
  key('Space');
  expect(editor.punctuationHotkeyStatus.textContent).toContain('Windows');
  confirmBinding();
  expect(updateConfig).toHaveBeenLastCalledWith('keybindings.toggle_punctuation', 'Ctrl+Space');
});
it('keeps only one recording or confirmation open across rows', () => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyC');
  editor.fullWidthHotkeyButton.dispatchEvent(new Event('click'));
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
  confirmBinding();
  expect(updateConfig).not.toHaveBeenCalled();
  editor.englishCandidatesHotkeyButton.dispatchEvent(new Event('click'));
  expect(editor.fullWidthHotkeyButton.getAttribute('aria-pressed')).toBe('false');
});
it('invalidates pending confirmation when another snapshot changes ownership', () => {
  editor.punctuationHotkeyButton.dispatchEvent(new Event('click'));
  key('KeyC');
  applyBinding('Ctrl+KeyC', 0);
  confirmBinding();
  expect(updateConfig).not.toHaveBeenCalled();
  expect(editor.punctuationHotkeyConfirmation.hidden).toBe(true);
});
it.each(['garbage', 'Alt+KeyJ', 'Alt+Shift+KeyJ'])('shows an invalid config binding %s until it is repaired', binding => {
  snapshot.actions[1].binding = binding;
  snapshot.actions[1].problem = '快捷键格式无效';
  applyShortcutConfig({ input_state_shortcuts: structuredClone(snapshot) });
  expect(editor.fullWidthHotkeyStatus.textContent).toContain('停用');
  editor.fullWidthHotkeyReset.dispatchEvent(new Event('click'));
  expect(updateConfig).toHaveBeenLastCalledWith('keybindings.toggle_full_width', 'Ctrl+Shift+Space');
});
