import { applyDropdownValue, applyToggleState, setupDropdownMenu, setupToggleButton } from './shared';
import { updateConfig } from './config-sync';
import { setupCredentialTest } from './credential-test';
import { setupModelFetch } from './model-fetch';
import { makeProviderId, sendProviderSync, type RegistryEntry } from './provider-sync';
import { type AiAssistantState, setAiAssistantState } from './ai-assistant-state';

type ProviderDefaults = { endpoint: string; model: string };

const fields: Record<string, string> = {
  aiToken: 'token', aiEndpoint: 'endpoint', aiModel: 'model'
};

const PROVIDER_DEFAULTS: Record<string, ProviderDefaults> = {
  deepseek: {
    endpoint: 'https://api.deepseek.com/chat/completions',
    model: 'deepseek-v4-flash'
  },
  openai: {
    endpoint: 'https://api.openai.com/v1/chat/completions',
    model: 'gpt-4o-mini'
  },
  siliconflow: {
    endpoint: 'https://api.siliconflow.cn/v1/chat/completions',
    model: 'Qwen/Qwen3-8B'
  },
  groq: {
    endpoint: 'https://api.groq.com/openai/v1/chat/completions',
    model: 'qwen/qwen3.8-27b'
  },
  minimax: {
    endpoint: 'https://api.minimax.cn/v1/chat/completions',
    model: 'MiniMax-M3.1-Flash-Preview'
  },
  moonshot: {
    endpoint: 'https://api.moonshot.cn/v1/chat/completions',
    model: 'kimi-k2-0905-preview'
  },
  zhipu: {
    endpoint: 'https://open.bigmodel.cn/api/paas/v4/chat/completions',
    model: 'glm-4-flash'
  },
  dashscope: {
    endpoint: 'https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions',
    model: 'qwen-plus'
  },
  volcengine: {
    endpoint: 'https://ark.cn-beijing.volces.com/api/v3/chat/completions',
    model: 'doubao-seed-1.6-flash'
  },
  openrouter: {
    endpoint: 'https://openrouter.ai/api/v1/chat/completions',
    model: 'openrouter/auto'
  },
  custom: {
    endpoint: '',
    model: ''
  }
};

const PROVIDERS = [
  'deepseek', 'openai', 'siliconflow', 'groq', 'minimax', 'moonshot', 'zhipu', 'dashscope',
  'volcengine', 'openrouter', 'custom'
] as const;
let tokens: Record<string, string> = {};
let endpoints: Record<string, string> = {};
let models: Record<string, string> = {};
// 服务商注册表：数据目录里 ai_providers.json 的镜像，HanaAgent 式的任意 OpenAI 兼容服务。
let registry: RegistryEntry[] = [];
let currentProvider = 'deepseek';
let currentPromptId = 'custom_1';
let customPrompts: Record<string, string> = { custom_1: '', custom_2: '', custom_3: '' };

function applyProviderFields(provider: string): void {
  const registered = registry.find(entry => entry.id === provider);
  const defaults = PROVIDER_DEFAULTS[provider] ?? { endpoint: registered?.endpoint ?? '', model: registered?.model ?? '' };
  const endpoint = document.getElementById('aiEndpoint') as HTMLInputElement | null;
  const model = document.getElementById('aiModel') as HTMLInputElement | null;
  // 空值和 Server 一致地表示提供商默认值；注册表里的条目就是自己的默认值。
  if (endpoint) endpoint.value = endpoints[provider] || registered?.endpoint || defaults.endpoint;
  if (model) {
    model.value = models[provider] || registered?.model || defaults.model;
    model.placeholder = defaults.model || registered?.model || '';
  }
  // 把字段标签标上当前服务商名：同屏还有「添加新服务商」的地址栏，不标名字分不清哪栏管哪个。
  const displayName = registered?.name || defaults_display_name(provider);
  const endpointLabel = document.getElementById('aiEndpointLabel');
  const tokenLabel = document.getElementById('aiTokenLabel');
  if (endpointLabel) endpointLabel.textContent = `接口地址（${displayName}）`;
  if (tokenLabel) tokenLabel.textContent = `API Token（${displayName}）`;
}

// 内置槽位的显示名。注册表条目用条目自己的 name。
const PROVIDER_DISPLAY_NAMES: Record<string, string> = {
  deepseek: 'DeepSeek', openai: 'OpenAI', siliconflow: 'SiliconFlow', groq: 'Groq', custom: 'Custom'
};

function defaults_display_name(provider: string): string {
  return PROVIDER_DISPLAY_NAMES[provider] ?? provider;
}

// 思考强度只有 DeepSeek 会发给服务商，其他服务商下隐藏，已保存的档位保留不动。
function applyThinkingVisibility(provider: string): void {
  const field = document.getElementById('aiThinkingField');
  if (field) field.hidden = provider !== 'deepseek';
}

function readProviderMap(raw: unknown, provider: string, legacyValue: string): Record<string, string> {
  const result: Record<string, string> = {};
  if (raw && typeof raw === 'object') {
    Object.entries(raw as Record<string, unknown>).forEach(([key, value]) => {
      if (typeof value === 'string') result[key] = value;
    });
  }
  if (legacyValue && !result[provider]) result[provider] = legacyValue;
  return result;
}

function switchProvider(provider: string): void {
  const token = document.getElementById('aiToken') as HTMLInputElement | null;
  if (token) {
    tokens[currentProvider] = token.value.trim();
    updateConfig(`ai_assistant.token_${currentProvider}`, tokens[currentProvider]);
  }
  for (const [id, key, slots] of [
    ['aiEndpoint', 'endpoint', endpoints], ['aiModel', 'model', models]
  ] as const) {
    const input = document.getElementById(id) as HTMLInputElement | null;
    if (input) {
      slots[currentProvider] = input.value;
      updateConfig(`ai_assistant.${key}`, input.value);
    }
  }
  currentProvider = provider;
  if (token) token.value = tokens[provider] ?? '';
  updateConfig('ai_assistant.provider', provider);
  applyProviderFields(provider);
  applyThinkingVisibility(provider);
  clearModelMenu();
  setAiAssistantState(currentAiConfig());
}

function switchPrompt(id: string): void {
  const prompt = document.getElementById('aiPrompt') as HTMLTextAreaElement | null;
  if (prompt) {
    customPrompts[currentPromptId] = prompt.value;
    updateConfig(`ai_assistant.prompt_${currentPromptId}`, prompt.value);
  }
  currentPromptId = id;
  if (prompt) prompt.value = customPrompts[id] ?? '';
}

// 凭据测试和模型列表共用同一份请求配置，空值按服务商默认值补全。
function currentAiConfig(): AiAssistantState {
  const defaults = PROVIDER_DEFAULTS[currentProvider];
  const value = (id: string) =>
    (document.getElementById(id) as HTMLInputElement | null)?.value.trim() ?? '';
  return {
    provider: currentProvider,
    token: value('aiToken'),
    endpoint: value('aiEndpoint') || defaults?.endpoint || '',
    model: value('aiModel') || defaults?.model || ''
  };
}

// 切换服务商后旧的模型列表和在途请求都不再适用，清空等待重新获取。
let clearModelMenu: () => void = () => {};

// 注册表驱动下拉菜单：条目即服务商，末尾固定一项「添加」。菜单是事件委托，动态重建不影响点击。
function rebuildProviderMenu(): void {
  const menu = document.getElementById('aiProviderMenu');
  if (!menu) return;
  menu.replaceChildren();
  registry.forEach(entry => {
    const item = document.createElement('div');
    item.className = 'dropdown-item';
    item.dataset.value = entry.id;
    item.textContent = PROVIDER_DISPLAY_NAMES[entry.id] || entry.name || entry.id;
    menu.appendChild(item);
  });
  const add = document.createElement('div');
  add.className = 'dropdown-item';
  add.dataset.value = '__add__';
  add.textContent = '＋ 添加服务商…';
  menu.appendChild(add);
}

function registryFromConfig(raw: unknown): RegistryEntry[] {
  if (!Array.isArray(raw)) return [];
  return raw.flatMap(item => {
    if (!item || typeof item !== 'object') return [];
    const entry = item as Record<string, unknown>;
    const field = (key: string): string => (typeof entry[key] === 'string' ? (entry[key] as string) : '');
    const id = field('id');
    if (!id) return [];
    return [{ id, name: field('name'), endpoint: field('endpoint'), token: field('token'), model: field('model') }];
  });
}

async function syncRegistry(entries: RegistryEntry[]): Promise<boolean> {
  const status = document.getElementById('aiProviderSyncStatus');
  if (status) {
    status.textContent = '正在保存…';
    status.dataset.kind = 'pending';
  }
  const ok = await sendProviderSync(entries, status);
  if (ok) registry = entries;
  rebuildProviderMenu();
  return ok;
}

function setupProviderManagement(): void {
  const form = document.getElementById('aiProviderAddForm');
  const nameInput = document.getElementById('aiProviderAddName') as HTMLInputElement | null;
  const endpointInput = document.getElementById('aiProviderAddEndpoint') as HTMLInputElement | null;
  const confirmButton = document.getElementById('aiProviderAddConfirm');
  const deleteButton = document.getElementById('aiProviderDeleteButton');
  const addButton = document.getElementById('aiProviderAddButton');

  const openForm = (): void => {
    form?.removeAttribute('hidden');
    nameInput?.focus();
  };
  addButton?.addEventListener('click', openForm);

  const submit = (): void => {
    const name = nameInput?.value.trim() ?? '';
    const endpoint = endpointInput?.value.trim() ?? '';
    if (!name || !/^https?:\/\//.test(endpoint)) {
      const status = document.getElementById('aiProviderSyncStatus');
      if (status) {
        status.textContent = '请填写名称和 https:// 开头的接口地址。';
        status.dataset.kind = 'error';
      }
      return;
    }
    const entry: RegistryEntry = { id: makeProviderId(name, registry), name, endpoint, token: '', model: '' };
    void syncRegistry([...registry, entry]).then(ok => {
      if (!ok) return;
      if (nameInput) nameInput.value = '';
      if (endpointInput) endpointInput.value = '';
      form?.setAttribute('hidden', '');
      // 直接切到新条目，用户接着填 Token 就行。
      switchProvider(entry.id);
    });
  };
  confirmButton?.addEventListener('click', submit);
  [nameInput, endpointInput].forEach(input => input?.addEventListener('keydown', (event) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      submit();
    }
    if (event.key === 'Escape') form?.setAttribute('hidden', '');
  }));

  deleteButton?.addEventListener('click', () => {
    if (registry.length <= 1) {
      const status = document.getElementById('aiProviderSyncStatus');
      if (status) {
        status.textContent = '至少保留一个服务商。';
        status.dataset.kind = 'error';
      }
      return;
    }
    const remaining = registry.filter(entry => entry.id !== currentProvider);
    void syncRegistry(remaining).then(ok => {
      if (!ok) return;
      switchProvider(remaining[0].id);
    });
  });
}

export function setupAiSettings(): void {
  setupToggleButton('aiEnabled', value => updateConfig('ai_assistant.enabled', value));
  setupTokenVisibilityToggle();
  setupCredentialTest('aiCredentialTestButton', 'aiCredentialTestStatus', () => 'ai.assistant', currentAiConfig);
  clearModelMenu = setupModelFetch({
    buttonId: 'aiModelFetchButton',
    statusId: 'aiModelFetchStatus',
    menuId: 'aiModelMenu',
    service: () => 'ai.assistant',
    readConfig: currentAiConfig
  });
  setupDropdownMenu('aiProviderBtn', 'aiProviderMenu', 'changeAiProvider', true);
  setupDropdownMenu('aiModelBtn', 'aiModelMenu', '', true, 'ai_assistant.model');
  setupModelArrowFetch();
  setupDropdownMenu('aiPromptSlotBtn', 'aiPromptSlotMenu', 'changeAiPromptSlot', true,
    'ai_assistant.prompt_id');
  setupDropdownMenu('aiThinkingBtn', 'aiThinkingMenu', 'changeAiThinking', true, 'ai_assistant.thinking');
  document.getElementById('aiProviderMenu')?.addEventListener('click', (event) => {
    const item = (event.target as HTMLElement | null)?.closest('.dropdown-item') as HTMLElement | null;
    const provider = item?.dataset.value;
    if (provider === '__add__') {
      // 末尾的「添加服务商」项：打开表单，不切换当前服务商。
      // 按钮标签要还原成当前服务商名，否则当前服务商看起来像被顶掉了。
      document.getElementById('aiProviderAddForm')?.removeAttribute('hidden');
      document.getElementById('aiProviderAddName')?.focus();
      applyDropdownValue('aiProviderBtn', 'aiProviderMenu', currentProvider);
      return;
    }
    if (provider) switchProvider(provider);
  });
  setupProviderManagement();
  // 从下拉列表选模型由通用下拉逻辑写配置和输入框，这里等它写完再同步给候选词翻译。
  document.getElementById('aiModelMenu')?.addEventListener('click', () => {
    queueMicrotask(() => setAiAssistantState(currentAiConfig()));
  });
  document.getElementById('aiPromptSlotMenu')?.addEventListener('click', (event) => {
    const item = (event.target as HTMLElement | null)?.closest('.dropdown-item') as HTMLElement | null;
    const id = item?.dataset.value;
    if (id) switchPrompt(id);
  });
  Object.entries(fields).forEach(([id, key]) => {
    const element = document.getElementById(id) as HTMLInputElement | HTMLTextAreaElement | null;
    element?.addEventListener('change', () => {
      if (id === 'aiToken') {
        tokens[currentProvider] = element.value.trim();
        updateConfig(`ai_assistant.token_${currentProvider}`, tokens[currentProvider]);
      } else {
        updateConfig(`ai_assistant.${key}`, element.value);
        if (!element.value) {
          const defaults = PROVIDER_DEFAULTS[currentProvider];
          if (defaults) element.value = defaults[key as keyof ProviderDefaults];
        }
      }
      setAiAssistantState(currentAiConfig());
    });
  });
  const limit = document.getElementById('aiCandidateLimit') as HTMLInputElement | null;
  limit?.addEventListener('change', () => {
    const value = Math.max(1, Math.min(10, Number.parseInt(limit.value, 10) || 3));
    limit.value = String(value);
    updateConfig('ai_assistant.candidate_limit', value);
  });
  const prompt = document.getElementById('aiPrompt') as HTMLTextAreaElement | null;
  prompt?.addEventListener('change', () => {
    customPrompts[currentPromptId] = prompt.value;
    updateConfig(`ai_assistant.prompt_${currentPromptId}`, prompt.value);
  });
}

// 还没有模型列表时点下拉箭头，等同点「查询模型」，结果返回后列表自动展开。
// 只挂在箭头上：点进输入框是要手动输入，不该发网络请求。
function setupModelArrowFetch(): void {
  const arrow = document.getElementById('aiModelBtn')?.querySelector<HTMLElement>('.arrow');
  const menu = document.getElementById('aiModelMenu');
  const fetchButton = document.getElementById('aiModelFetchButton') as HTMLButtonElement | null;
  if (!arrow || !menu || !fetchButton) return;

  arrow.addEventListener('click', () => {
    if (!menu.querySelector('.dropdown-item')) fetchButton.click();
  });
}

function setupTokenVisibilityToggle(): void {
  const token = document.getElementById('aiToken') as HTMLInputElement | null;
  const toggle = document.getElementById('aiTokenVisibility') as HTMLButtonElement | null;
  if (!token || !toggle) return;

  toggle.addEventListener('click', () => {
    const shouldShow = token.type === 'password';
    token.type = shouldShow ? 'text' : 'password';
    toggle.setAttribute('aria-pressed', String(shouldShow));

    const label = shouldShow ? '隐藏 API Token' : '显示 API Token';
    toggle.setAttribute('aria-label', label);
    toggle.title = label;
  });
}

export function applyAiConfig(config: Record<string, unknown>): void {
  Object.entries(fields).forEach(([id, key]) => {
    const element = document.getElementById(id) as HTMLInputElement | HTMLTextAreaElement | null;
    if (element && typeof config[key] === 'string') element.value = config[key] as string;
  });
  const limit = document.getElementById('aiCandidateLimit') as HTMLInputElement | null;
  if (limit && typeof config.candidate_limit === 'number') limit.value = String(config.candidate_limit);
  if (typeof config.enabled === 'boolean') applyToggleState('aiEnabled', config.enabled);
  applyDropdownValue('aiThinkingBtn', 'aiThinkingMenu',
    typeof config.thinking === 'string' ? config.thinking : 'disabled');
  currentProvider = typeof config.provider === 'string' ? config.provider : 'deepseek';
  tokens = readProviderMap(config.tokens, currentProvider, typeof config.token === 'string' ? config.token : '');
  endpoints = readProviderMap(config.endpoints, currentProvider, typeof config.endpoint === 'string' ? config.endpoint : '');
  models = readProviderMap(config.models, currentProvider, typeof config.model === 'string' ? config.model : '');
  PROVIDERS.forEach((provider) => {
    const slot = config[`token_${provider}`];
    if (typeof slot === 'string' && slot) tokens[provider] = slot;
  });
  // 注册表优先：快照里的 providers 数组就是数据目录那份 ai_providers.json。
  registry = registryFromConfig(config.providers);
  if (registry.length > 0) {
    registry.forEach(entry => {
      if (entry.endpoint) endpoints[entry.id] = entry.endpoint;
      if (entry.model) models[entry.id] = entry.model;
      if (entry.token) tokens[entry.id] = entry.token;
    });
    rebuildProviderMenu();
    if (!registry.some(entry => entry.id === currentProvider)) currentProvider = registry[0].id;
  }
  const token = document.getElementById('aiToken') as HTMLInputElement | null;
  if (token) token.value = tokens[currentProvider] ?? '';
  applyDropdownValue('aiProviderBtn', 'aiProviderMenu', currentProvider);
  applyThinkingVisibility(currentProvider);
  const defaults = PROVIDER_DEFAULTS[currentProvider];
  const model = document.getElementById('aiModel') as HTMLInputElement | null;
  if (model && defaults) model.placeholder = defaults.model;
  applyProviderFields(currentProvider);
  currentPromptId = typeof config.prompt_id === 'string' ? config.prompt_id : 'custom_1';
  customPrompts = {
    custom_1: typeof config.prompt_custom_1 === 'string'
      ? config.prompt_custom_1
      : (typeof config.prompt === 'string' ? config.prompt : ''),
    custom_2: typeof config.prompt_custom_2 === 'string' ? config.prompt_custom_2 : '',
    custom_3: typeof config.prompt_custom_3 === 'string' ? config.prompt_custom_3 : ''
  };
  applyDropdownValue('aiPromptSlotBtn', 'aiPromptSlotMenu', currentPromptId);
  const prompt = document.getElementById('aiPrompt') as HTMLTextAreaElement | null;
  if (prompt) prompt.value = customPrompts[currentPromptId] ?? '';
}
