#!/usr/bin/env node
// 冻结资产绑定的构建期校验器（规格 §5.2 / 验收 DD-SYS-04）。
//
// 失败即中止构建，避免“配置写错但界面静默不可用”。校验内容：
//   1. schema_version 与必填字段、枚举
//   2. asset_id / scene_instance / device_id / zone 唯一性
//   3. verified 绑定必须携带真实 device_id
//   4. scene_instance 与 zone scene_root 必须真实存在于工厂 GLB
//   5. public/models 下的模型与 assets.lock.json 的 SHA-256 一致
// 通过后生成服务端可读副本 server/asset-bindings.json。

import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const uiDir = path.resolve(scriptDir, '..');
const repoDir = path.resolve(uiDir, '..', '..');

const BINDINGS_PATH = path.join(uiDir, 'src', 'config', 'asset-bindings.json');
const LOCK_PATH = path.join(uiDir, 'assets.lock.json');
const SERVER_COPY_PATH = path.join(repoDir, 'server', 'asset-bindings.json');
const FACTORY_MODEL = path.join(uiDir, 'public', 'models', 'factory-web.glb');

const BINDING_STATES = ['verified', 'provisional', 'unbound'];
const BINDING_FIELDS = [
  'asset_id', 'device_id', 'scene_instance', 'zone',
  'detail_model', 'sensor_node', 'display_name_zh', 'binding_state',
];

const problems = [];
const fail = (message) => problems.push(message);

function readJson(file) {
  try {
    return JSON.parse(readFileSync(file, 'utf8'));
  } catch (error) {
    console.error(`[bindings] 无法读取 ${path.relative(repoDir, file)}: ${error.message}`);
    process.exit(1);
  }
}

// 从 GLB 的 JSON chunk 里取出节点名，用于校验 scene_instance 真实存在。
function glbNodeNames(file) {
  const buffer = readFileSync(file);
  if (buffer.readUInt32LE(0) !== 0x46546c67) throw new Error('不是 GLB 文件');
  let offset = 12;
  while (offset + 8 <= buffer.length) {
    const chunkLength = buffer.readUInt32LE(offset);
    const chunkType = buffer.readUInt32LE(offset + 4);
    if (chunkType === 0x4e4f534a) {
      const json = JSON.parse(buffer.subarray(offset + 8, offset + 8 + chunkLength).toString('utf8'));
      return new Set((json.nodes ?? []).map((node) => node.name).filter(Boolean));
    }
    offset += 8 + chunkLength;
  }
  throw new Error('GLB 中缺少 JSON chunk');
}

function sha256(file) {
  return createHash('sha256').update(readFileSync(file)).digest('hex');
}

const config = readJson(BINDINGS_PATH);
const lock = readJson(LOCK_PATH);

if (config.schema_version !== 1) fail(`asset-bindings.json schema_version 必须为 1，当前 ${config.schema_version}`);
if (!Array.isArray(config.assets) || config.assets.length === 0) fail('asset-bindings.json 缺少 assets 数组');
if (!Array.isArray(config.zones) || config.zones.length === 0) fail('asset-bindings.json 缺少 zones 数组');
if (problems.length) {
  problems.forEach((message) => console.error(`[bindings] ${message}`));
  process.exit(1);
}

// 场景节点：GLB 存在性校验
if (!existsSync(FACTORY_MODEL)) fail(`缺少工厂模型 ${path.relative(repoDir, FACTORY_MODEL)}`);
const nodeNames = existsSync(FACTORY_MODEL) ? glbNodeNames(FACTORY_MODEL) : new Set();

const zoneNames = new Set();
for (const zone of config.zones) {
  if (typeof zone.zone !== 'string' || !/^[A-F]$/.test(zone.zone)) fail(`zone 编号非法: ${JSON.stringify(zone.zone)}`);
  if (zoneNames.has(zone.zone)) fail(`zone 重复: ${zone.zone}`);
  zoneNames.add(zone.zone);
  if (typeof zone.display_name_zh !== 'string' || !zone.display_name_zh) fail(`zone ${zone.zone} 缺少 display_name_zh`);
  if (typeof zone.scene_root !== 'string' || !zone.scene_root) fail(`zone ${zone.zone} 缺少 scene_root`);
  else if (!nodeNames.has(zone.scene_root)) fail(`zone ${zone.zone} 的 scene_root「${zone.scene_root}」不在工厂模型中`);
}

const seen = { asset_id: new Map(), scene_instance: new Map(), device_id: new Map() };
for (const [index, binding] of config.assets.entries()) {
  const where = `assets[${index}]`;
  for (const field of BINDING_FIELDS) {
    if (!(field in binding)) fail(`${where} 缺少字段 ${field}`);
  }
  if (problems.length) continue;

  const { asset_id: assetId, device_id: deviceId, scene_instance: instance, zone, binding_state: state } = binding;
  if (typeof assetId !== 'string' || !/^[a-z0-9][a-z0-9-]*$/.test(assetId)) fail(`${where}.asset_id 非法: ${JSON.stringify(assetId)}`);
  if (typeof instance !== 'string' || !instance) fail(`${where}.scene_instance 非法: ${JSON.stringify(instance)}`);
  else if (!nodeNames.has(instance)) fail(`${where}.scene_instance「${instance}」不在工厂模型中`);
  if (typeof binding.display_name_zh !== 'string' || !binding.display_name_zh) fail(`${where}.display_name_zh 不能为空`);
  if (binding.detail_model !== null && typeof binding.detail_model !== 'string') fail(`${where}.detail_model 必须为字符串或 null`);
  if (binding.sensor_node !== null && typeof binding.sensor_node !== 'string') fail(`${where}.sensor_node 必须为字符串或 null`);
  if (!BINDING_STATES.includes(state)) fail(`${where}.binding_state 非法: ${JSON.stringify(state)}`);
  if (!zoneNames.has(zone)) fail(`${where}.zone「${zone}」未在 zones 中声明`);

  // verified 必须指向真实设备；unbound 表示模型尚未绑定任何设备。
  // provisional 允许填写 device_id，但服务端只对 verified 计算在线状态。
  if (state === 'verified') {
    if (typeof deviceId !== 'string' || !deviceId) fail(`${where} 为 verified 绑定，必须提供 device_id`);
  } else if (state === 'unbound' && deviceId !== null) {
    fail(`${where} 为 unbound 绑定，device_id 必须为 null`);
  } else if (deviceId !== null && typeof deviceId !== 'string') {
    fail(`${where}.device_id 必须为字符串或 null`);
  }

  for (const [field, value] of [['asset_id', assetId], ['scene_instance', instance], ['device_id', deviceId]]) {
    if (value === null || value === undefined) continue;
    if (seen[field].has(value)) fail(`${field} 重复: ${value}（${seen[field].get(value)} 与 ${where}）`);
    else seen[field].set(value, where);
  }
}

// 模型完整性：与 assets.lock.json 冻结的 SHA-256 一致
for (const entry of lock.assets ?? []) {
  const file = path.join(uiDir, entry.path);
  if (!existsSync(file)) {
    fail(`assets.lock.json 声明的文件缺失: ${entry.path}`);
    continue;
  }
  const digest = sha256(file);
  if (digest !== entry.sha256) fail(`${entry.path} SHA-256 不匹配，期望 ${entry.sha256}，实际 ${digest}`);
}

if (problems.length) {
  console.error(`[bindings] 校验失败，共 ${problems.length} 项：`);
  problems.forEach((message) => console.error(`  - ${message}`));
  process.exit(1);
}

writeFileSync(SERVER_COPY_PATH, `${JSON.stringify(config, null, 2)}\n`, 'utf8');
const verified = config.assets.filter((binding) => binding.binding_state === 'verified');
const provisional = config.assets.filter((binding) => binding.binding_state === 'provisional');
console.log(
  `[bindings] 校验通过：${config.assets.length} 项绑定（verified ${verified.length} / provisional ${provisional.length} / unbound ${config.assets.length - verified.length - provisional.length}），`
  + `${config.zones.length} 个分区；已生成 ${path.relative(repoDir, SERVER_COPY_PATH)}`,
);
