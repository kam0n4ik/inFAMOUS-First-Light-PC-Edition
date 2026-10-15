// FUN_00300b00 @0x300b00 size=699
// callers: 0x18e210 0x18e4c0 0x1f0be0 0x300a90
// callees: 0x2ffeb0(FUN_002ffeb0) 0x3010a0(FUN_003010a0) 0x984360(__stack_chk_fail)

void FUN_00300b00(undefined8 *param_1)

{
  undefined8 *puVar1;
  int iVar2;
  undefined8 uVar3;
  undefined8 uVar4;
  undefined8 uVar5;
  undefined8 uVar6;
  undefined8 uVar7;
  undefined8 uVar8;
  undefined8 uVar9;
  undefined8 uVar10;
  undefined8 uVar11;
  undefined8 uVar12;
  undefined8 uVar13;
  undefined8 uVar14;
  undefined8 uVar15;
  undefined8 uVar16;
  undefined8 uVar17;
  long lVar18;
  ulong uVar19;
  undefined8 *puVar20;
  undefined1 (*pauVar21) [32];
  undefined8 *puVar22;
  uint uVar23;
  uint *puVar25;
  undefined1 auVar26 [32];
  undefined8 local_888;
  undefined8 uStack_880;
  undefined4 uStack_878;
  undefined4 uStack_874;
  undefined4 uStack_870;
  undefined4 uStack_86c;
  undefined4 uStack_868;
  undefined4 uStack_864;
  undefined8 local_860 [234];
  uint local_110;
  undefined4 uStack_10c;
  undefined4 uStack_108;
  undefined4 uStack_104;
  undefined4 local_100;
  uint local_f8 [16];
  long local_b8 [16];
  long local_38;
  ulong uVar24;
  
  local_38 = s__libexec_ld_elf_so_1_00000000._0_8_;
  lVar18 = (long)*(int *)(param_1 + 1);
  if ((lVar18 == -1) ||
     (*(int *)(&DAT_01d0cbe8 + lVar18 * 8) == 0 && *(int *)(&DAT_01d0cbe4 + lVar18 * 8) == 0))
  goto LAB_00300da3;
  iVar2 = *(int *)(param_1 + 0xef);
  if (0 < (long)iVar2) {
    puVar25 = (uint *)((long)param_1 + 0xf4);
    lVar18 = 0;
    do {
      uVar23 = *puVar25;
      if ((uVar23 & 2) == 0) {
        local_b8[lVar18] = (long)(puVar25 + -0x33);
      }
      puVar25 = puVar25 + 0x34;
      local_f8[lVar18] = uVar23 & 0xffffffe3 | 0x10;
      lVar18 = lVar18 + 1;
    } while (lVar18 < iVar2);
  }
  local_888 = 0;
  uStack_880 = 0xffffffff;
  uStack_878 = 0;
  uStack_874 = 0xbf800000;
  uStack_870 = 0xbf800000;
  uStack_86c = 0x3f800000;
  uStack_868 = 0x3f800000;
  uStack_864 = 0;
  local_110 = 0;
  uStack_10c = 0;
  uStack_108 = 0xffffffff;
  uStack_104 = 0xffffffff;
  local_100 = 0xffffffff;
  FUN_002ffeb0(&local_888);
  uStack_864 = *(undefined4 *)((long)param_1 + 0x24);
  uStack_880 = CONCAT44(uStack_880._4_4_,*(undefined4 *)(param_1 + 1));
  param_1[4] = CONCAT44(uStack_864,uStack_868);
  *param_1 = local_888;
  param_1[1] = uStack_880;
  param_1[2] = CONCAT44(uStack_874,uStack_878);
  param_1[3] = CONCAT44(uStack_86c,uStack_870);
  uVar19 = (ulong)local_110;
  if (&local_888 == param_1) {
LAB_00300d08:
    *(ulong *)((long)param_1 + 0x77c) = CONCAT44(uStack_108,uStack_10c);
    *(ulong *)((long)param_1 + 0x784) = CONCAT44(local_100,uStack_104);
    if (0 < (int)local_110) {
      lVar18 = 0;
      do {
        FUN_003010a0();
        uVar19 = (ulong)(int)local_110;
        lVar18 = lVar18 + 1;
      } while (lVar18 < (long)uVar19);
    }
  }
  else {
    *(uint *)(param_1 + 0xef) = local_110;
    if (0 < (int)local_110) {
      puVar20 = local_860;
      puVar22 = param_1 + 5;
      uVar24 = uVar19;
      do {
        uVar3 = puVar20[0x17];
        uVar4 = puVar20[0x18];
        uVar5 = puVar20[0x19];
        puVar22[0x16] = puVar20[0x16];
        puVar22[0x17] = uVar3;
        puVar22[0x18] = uVar4;
        puVar22[0x19] = uVar5;
        uVar3 = puVar20[0x15];
        uVar4 = puVar20[0x16];
        uVar5 = puVar20[0x17];
        puVar22[0x14] = puVar20[0x14];
        puVar22[0x15] = uVar3;
        puVar22[0x16] = uVar4;
        puVar22[0x17] = uVar5;
        uVar3 = puVar20[0x11];
        uVar4 = puVar20[0x12];
        uVar5 = puVar20[0x13];
        puVar22[0x10] = puVar20[0x10];
        puVar22[0x11] = uVar3;
        puVar22[0x12] = uVar4;
        puVar22[0x13] = uVar5;
        uVar7 = *puVar20;
        uVar8 = puVar20[1];
        uVar9 = puVar20[2];
        uVar10 = puVar20[3];
        uVar3 = puVar20[4];
        uVar4 = puVar20[5];
        uVar5 = puVar20[6];
        uVar6 = puVar20[7];
        uVar11 = puVar20[8];
        uVar12 = puVar20[9];
        uVar13 = puVar20[10];
        uVar14 = puVar20[0xb];
        puVar1 = puVar20 + 0xc;
        uVar15 = puVar20[0xd];
        uVar16 = puVar20[0xe];
        uVar17 = puVar20[0xf];
        puVar20 = puVar20 + 0x1a;
        puVar22[0xc] = *puVar1;
        puVar22[0xd] = uVar15;
        puVar22[0xe] = uVar16;
        puVar22[0xf] = uVar17;
        puVar22[8] = uVar11;
        puVar22[9] = uVar12;
        puVar22[10] = uVar13;
        puVar22[0xb] = uVar14;
        puVar22[4] = uVar3;
        puVar22[5] = uVar4;
        puVar22[6] = uVar5;
        puVar22[7] = uVar6;
        *puVar22 = uVar7;
        puVar22[1] = uVar8;
        puVar22[2] = uVar9;
        puVar22[3] = uVar10;
        puVar22 = puVar22 + 0x1a;
        uVar23 = (int)uVar24 - 1;
        uVar24 = (ulong)uVar23;
      } while (uVar23 != 0);
      goto LAB_00300d08;
    }
    *(ulong *)((long)param_1 + 0x77c) = CONCAT44(uStack_108,uStack_10c);
    *(ulong *)((long)param_1 + 0x784) = CONCAT44(local_100,uStack_104);
  }
  if ((int)uVar19 < 0) {
    auVar26 = ZEXT1232(ZEXT812(0));
    pauVar21 = (undefined1 (*) [32])(&local_888 + (long)(int)uVar19 * 0x1a + 5);
    do {
      *(undefined1 (*) [32])((long)pauVar21[3] + 0xc) = auVar26;
      pauVar21[3] = auVar26;
      pauVar21[2] = auVar26;
      pauVar21[1] = auVar26;
      *pauVar21 = auVar26;
      pauVar21 = (undefined1 (*) [32])((long)pauVar21[6] + 0x10);
      uVar23 = (int)uVar19 + 1;
      uVar19 = (ulong)uVar23;
    } while (uVar23 != 0);
  }
LAB_00300da3:
  if (s__libexec_ld_elf_so_1_00000000._0_8_ != local_38) {
    ps4import::__stack_chk_fail();
    return;
  }
  return;
}

