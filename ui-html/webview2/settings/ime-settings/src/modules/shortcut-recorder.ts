import { updateConfig } from './config-sync';

interface ShortcutAction {
  key: string; label: string; editor: string; defaultBinding: string; binding: string; problem: string;
}
interface Rule { binding: string; label: string }
export interface ShortcutMetadata {
  actions: ShortcutAction[]; reserved: Rule[]; warnings: Rule[];
  keys: { code: string; vk: number; label: string }[];
}
interface Editor { finish: (message?: string) => void; render: () => void }
let metadata: ShortcutMetadata | undefined;
let editors = new Map<string, Editor>();
let active: Editor | undefined;
let saving = false;
let ready = false;
// The host echoes a snapshot after every write; without one, give the editors back.
const SAVE_TIMEOUT_MS = 5000;
let savingTimer: ReturnType<typeof setTimeout> | undefined;

function endSaving(): void {
  saving = false;
  clearTimeout(savingTimer);
  savingTimer = undefined;
}

function format(binding: string): string {
  return binding ? binding.split('+').map(key => metadata?.keys.find(k => k.code === key)?.label ??
    key.replace(/^(Key|Digit)/, '')).join(' + ') : '未绑定';
}
function eventCode(event: KeyboardEvent): string {
  if (/^(Control|Shift|Alt|Meta)(Left|Right)$/.test(event.code)) return event.code;
  if (event.code.startsWith('Numpad') || event.location === 3) return '';
  const vk = event.keyCode;
  if (vk >= 65 && vk <= 90) return `Key${String.fromCharCode(vk)}`;
  if (vk >= 48 && vk <= 57) return `Digit${vk - 48}`;
  if (vk >= 112 && vk <= 123) return `F${vk - 111}`;
  return metadata?.keys.find(key => key.vk === vk)?.code ??
    ({ 9: 'Tab', 16: 'ShiftLeft', 17: 'ControlLeft', 18: 'AltLeft', 27: 'Escape',
      91: 'MetaLeft', 92: 'MetaRight' } as Record<number, string>)[vk] ?? '';
}
function conflict(action: ShortcutAction, binding: string): string | undefined {
  if (!binding) return;
  return metadata?.reserved.find(rule => rule.binding === binding)?.label ??
    metadata?.actions.find(other => other.key !== action.key && other.binding === binding)?.label;
}
function createEditor(key: string, prefix: string): Editor | undefined {
  const button = document.getElementById(`${prefix}Button`) as HTMLButtonElement | null;
  const status = document.getElementById(`${prefix}Status`);
  const confirmation = document.getElementById(`${prefix}Confirmation`);
  if (!button) return;
  let pending: string | undefined;
  const action = () => metadata!.actions.find(item => item.key === key)!;
  const render = () => {
    if (button.getAttribute('aria-pressed') !== 'true') button.textContent = format(action().binding);
    for (const suffix of ['Button', 'Clear', 'Reset']) {
      const control = document.getElementById(`${prefix}${suffix}`) as HTMLButtonElement | null;
      if (control) control.disabled = saving;
    }
  };
  const finish = (message = '') => {
    pending = undefined;
    if (active === editor) active = undefined;
    if (confirmation) confirmation.hidden = true;
    button.setAttribute('aria-pressed', 'false');
    render();
    if (status) status.textContent = message;
  };
  const editor = { finish, render };
  const save = (binding: string, confirmed = false) => {
    if (saving) return;
    if (active && active !== editor) active.finish();
    const owner = conflict(action(), binding);
    if (owner) {
      pending = undefined;
      if (confirmation) confirmation.hidden = true;
      if (status) status.textContent = `${format(binding)} 用于${owner}。`;
      return;
    }
    const warning = metadata?.warnings.find(rule => rule.binding === binding)?.label;
    if (warning && !confirmed) {
      finish(`${format(binding)} 与常用的“${warning}”快捷键冲突，绑定后会覆盖当前软件的这项操作。仍要绑定吗？`);
      pending = binding;
      active = editor;
      if (confirmation) confirmation.hidden = false;
      return;
    }
    finish('正在保存…');
    saving = true;
    savingTimer = setTimeout(() => {
      endSaving();
      editors.forEach(item => item.render());
      finish('保存未完成，请重试。');
    }, SAVE_TIMEOUT_MS);
    editors.forEach(item => item.render());
    updateConfig(`keybindings.${key}`, binding);
  };
  button.addEventListener('click', () => {
    if (saving) return;
    if (button.getAttribute('aria-pressed') === 'true') return finish();
    active?.finish();
    finish();
    active = editor;
    button.setAttribute('aria-pressed', 'true');
    button.textContent = '请按下快捷键';
    if (status) status.textContent = 'Ctrl + 字母、数字、F1–F12、标点、Space，可加 Shift；Esc 取消，Tab 离开。';
  });
  button.addEventListener('blur', () => {
    if (button.getAttribute('aria-pressed') === 'true') finish();
  });
  button.addEventListener('keydown', event => {
    const code = eventCode(event);
    if (pending !== undefined && code === 'Escape') {
      event.preventDefault(); event.stopPropagation(); return finish();
    }
    if (button.getAttribute('aria-pressed') !== 'true' || code === 'Tab') return;
    event.preventDefault(); event.stopPropagation();
    if (code === 'Escape') return finish();
    if (event.repeat || /^(Control|Shift|Alt|Meta)(Left|Right)$/.test(code)) return;
    if (event.metaKey || !event.ctrlKey || event.altKey || event.getModifierState('AltGraph') ||
        (!/^(Key[A-Z]|Digit[0-9]|F([1-9]|1[0-2]))$/.test(code) && !metadata?.keys.some(k => k.code === code))) {
      if (status) status.textContent = '请选择 Ctrl + 字母、数字、F1–F12、标点、Space，可加 Shift。';
      return;
    }
    save(`Ctrl+${event.shiftKey ? 'Shift+' : ''}${code}`);
  });
  document.getElementById(`${prefix}Confirm`)?.addEventListener('click', () => {
    if (pending !== undefined) save(pending, true);
  });
  document.getElementById(`${prefix}Cancel`)?.addEventListener('click', () => finish());
  confirmation?.addEventListener('keydown', event => {
    if (event.key === 'Escape') {
      event.preventDefault(); event.stopPropagation(); finish(); button.focus();
    }
  });
  document.getElementById(`${prefix}Clear`)?.addEventListener('click', () => save(''));
  document.getElementById(`${prefix}Reset`)?.addEventListener('click', () => save(action().defaultBinding));
  finish(action().problem ? `此快捷键已停用：${action().problem}。请重新绑定。` : '');
  return editor;
}
function ensureEditors(): void {
  if (!ready || !metadata) return;
  for (const action of metadata.actions) {
    if (!editors.has(action.key)) {
      const editor = createEditor(action.key, action.editor);
      if (editor) editors.set(action.key, editor);
    }
  }
}
export function setupInputStateShortcuts(): void {
  editors = new Map(); active = undefined; endSaving(); ready = true;
  ensureEditors();
}
export function applyInputStateShortcuts(value?: ShortcutMetadata): void {
  if (!value) return;
  const changed = JSON.stringify(metadata) !== JSON.stringify(value);
  metadata = value;
  endSaving();
  ensureEditors();
  if (changed) active?.finish();
  editors.forEach((editor, key) => {
    const action = metadata!.actions.find(item => item.key === key)!;
    if (action.problem) editor.finish(`此快捷键已停用：${action.problem}。请重新绑定。`);
    else if (changed || !active) editor.finish();
    else editor.render();
  });
}
