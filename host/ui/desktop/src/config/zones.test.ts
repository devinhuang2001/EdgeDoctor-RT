import { describe, expect, it } from 'vitest';
import { bindingForAsset, frozenBindings, zoneDefinitions, zoneMeta } from './zones';

describe('冻结的资产绑定配置', () => {
  it('声明六个分区且编号唯一', () => {
    expect(zoneDefinitions).toHaveLength(6);
    expect(zoneDefinitions.map((zone) => zone.zone)).toEqual(['A', 'B', 'C', 'D', 'E', 'F']);
    expect(new Set(zoneDefinitions.map((zone) => zone.scene_root)).size).toBe(6);
  });

  it('每个绑定都指向已声明的分区', () => {
    const zones = new Set(zoneDefinitions.map((zone) => zone.zone));
    frozenBindings.forEach((binding) => expect(zones.has(binding.zone)).toBe(true));
  });

  it('资产、设备与场景实例都不重复', () => {
    expect(new Set(frozenBindings.map((binding) => binding.asset_id)).size).toBe(frozenBindings.length);
    expect(new Set(frozenBindings.map((binding) => binding.scene_instance)).size).toBe(frozenBindings.length);
    const devices = frozenBindings.map((binding) => binding.device_id).filter((id): id is string => Boolean(id));
    expect(new Set(devices).size).toBe(devices.length);
  });

  it('只有已验证绑定携带真实设备', () => {
    frozenBindings.forEach((binding) => {
      if (binding.binding_state === 'verified') expect(binding.device_id).toBeTruthy();
      if (binding.binding_state === 'unbound') expect(binding.device_id).toBeNull();
    });
    expect(frozenBindings.filter((binding) => binding.binding_state === 'verified')).toHaveLength(1);
  });

  it('电机绑定提供详情模型与传感器节点', () => {
    const motor = bindingForAsset('motor-01');
    expect(motor?.scene_instance).toBe('MOTOR_01');
    expect(motor?.detail_model).toBe('models/induction_motor.glb');
    expect(motor?.sensor_node).toBe('IMU_Sensor');
  });

  it('未知分区返回可读的兜底元数据', () => {
    expect(zoneMeta('Z').display_name_zh).toBe('区域 Z');
  });
});
