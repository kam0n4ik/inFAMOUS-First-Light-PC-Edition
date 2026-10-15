// FUN_001f1210 @0x1f1210 size=368
// callers: 
// callees: 

void FUN_001f1210(undefined8 *param_1)

{
  uint uVar1;
  uint uVar2;
  uint uVar3;
  uint uVar4;
  uint uVar5;
  undefined1 auVar6 [16];
  
  *(int *)((long)param_1 + 0xdf0f14) = (int)*param_1;
  *(int *)(param_1 + 0x1be1e3) = (int)((ulong)*param_1 >> 0x20);
  uVar1 = *(uint *)((long)param_1 + 0xc);
  uVar5 = *(uint *)(param_1 + 2);
  *(uint *)((long)param_1 + 0xdf0f1c) = uVar1;
  *(uint *)(param_1 + 0x1be1e4) = uVar5;
  auVar6 = vpshufd_avx(ZEXT116(*(byte *)((long)param_1 + 0xdf0f05) == 0),0);
  uVar4 = uVar1 >> (*(byte *)((long)param_1 + 0xdf0f05) & 0x1f);
  auVar6 = vpslld_avx(auVar6,0x1f);
  uVar2 = uVar5 >> 1;
  auVar6 = vpsrad_avx(auVar6,0x1f);
  uVar3 = uVar4 >> 1;
  auVar6 = vpand_avx(auVar6,ZEXT416(uVar1));
  *(undefined1 (*) [16])((long)param_1 + 0xdf0f64) = auVar6;
  *(uint *)((long)param_1 + 0xdf0f24) = uVar4;
  *(uint *)(param_1 + 0x1be1e5) = uVar5;
  *(uint *)((long)param_1 + 0xdf0f74) = uVar3;
  *(uint *)(param_1 + 0x1be1ef) = uVar2;
  if (DAT_01d0cbd5 != '\0') {
    uVar2 = uVar5 >> 2;
  }
  *(uint *)((long)param_1 + 0xdf0f2c) = uVar3;
  *(uint *)(param_1 + 0x1be1e6) = uVar2;
  uVar1 = 0x1e0;
  if (0x1e0 < uVar4 >> 2) {
    uVar1 = uVar4 >> 2;
  }
  *(uint *)((long)param_1 + 0xdf0f5c) = (uVar3 + 0x13) / 0x14;
  *(uint *)(param_1 + 0x1be1ec) = (uVar2 + 0x13) / 0x14;
  uVar5 = 0x10e;
  if (0x10e < uVar2 >> 1) {
    uVar5 = uVar2 >> 1;
  }
  *(uint *)((long)param_1 + 0xdf0f34) = uVar1;
  *(uint *)(param_1 + 0x1be1e7) = uVar5;
  *(uint *)((long)param_1 + 0xdf0f3c) = uVar1 >> 1;
  *(uint *)(param_1 + 0x1be1e8) = uVar5 >> 1;
  *(uint *)((long)param_1 + 0xdf0f44) = uVar1 >> 2;
  *(uint *)(param_1 + 0x1be1e9) = uVar5 >> 2;
  *(uint *)((long)param_1 + 0xdf0f4c) = uVar1 >> 3;
  *(uint *)(param_1 + 0x1be1ea) = uVar5 >> 3;
  *(uint *)((long)param_1 + 0xdf0f54) = uVar1 >> 4;
  *(uint *)(param_1 + 0x1be1eb) = uVar5 >> 4;
  return;
}

