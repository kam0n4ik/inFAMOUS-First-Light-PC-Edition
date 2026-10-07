# Second Son — импорты eboot.bin (сгенерировано tools/audit_imports.py)

Лог рантайма: `logs\reference\trace_lang1.txt`

`seen` — сколько раз имя функции встретилось в логе shadPS4 (0 ≠ «не вызывается»: многие HLE-функции ничего не логируют).

| Модуль | Всего | IMPLEMENTED | LLE | PARTIAL | TRIVIAL_RETURN | DUMMY | STUBBED | UNIMPLEMENTED | NO_BODY_FOUND | OBJECT | MISSING | seen>0 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| libSceAjm | 11 | 8 |  |  |  |  | 3 |  |  |  |  | 2 |
| libSceAppContentUtil | 2 | 1 |  | 1 |  |  |  |  |  |  |  | 2 |
| libSceAudioOut | 6 | 4 |  | 2 |  |  |  |  |  |  |  | 2 |
| libSceCommonDialog | 1 | 1 |  |  |  |  |  |  |  |  |  | 1 |
| libSceErrorDialog | 4 | 4 |  |  |  |  |  |  |  |  |  | 0 |
| libSceFios2 | 41 |  | 41 |  |  |  |  |  |  |  |  | 0 |
| libSceGnmDriver | 85 | 55 |  |  | 14 |  | 16 |  |  |  |  | 5 |
| libSceHttp | 18 | 17 |  |  |  |  | 1 |  |  |  |  | 6 |
| libSceMsgDialog | 3 | 3 |  |  |  |  |  |  |  |  |  | 0 |
| libSceNet | 30 | 29 |  |  | 1 |  |  |  |  |  |  | 2 |
| libSceNetCtl | 6 | 3 |  |  |  |  | 3 |  |  |  |  | 3 |
| libSceNpManager | 14 | 12 |  | 1 |  |  | 1 |  |  |  |  | 5 |
| libSceNpTrophy | 7 | 7 |  |  |  |  |  |  |  |  |  | 4 |
| libScePad | 7 | 6 |  |  |  |  | 1 |  |  |  |  | 5 |
| libScePlayGo | 10 | 10 |  |  |  |  |  |  |  |  |  | 7 |
| libSceSaveData | 8 | 8 |  |  |  |  |  |  |  |  |  | 6 |
| libSceSaveDataDialog | 5 | 5 |  |  |  |  |  |  |  |  |  | 0 |
| libSceSsl | 2 |  |  |  |  | 1 | 1 |  |  |  |  | 1 |
| libSceSysmodule | 2 | 1 |  | 1 |  |  |  |  |  |  |  | 1 |
| libSceSystemService | 6 | 5 |  |  |  |  | 1 |  |  |  |  | 4 |
| libSceUserService | 5 | 3 |  |  |  | 1 | 1 |  |  |  |  | 4 |
| libSceVideoOut | 13 | 13 |  |  |  |  |  |  |  |  |  | 2 |
| libSceVideodec | 6 | 4 |  | 2 |  |  |  |  |  |  |  | 5 |
| libc | 101 |  | 101 |  |  |  |  |  |  |  |  | 0 |
| libkernel | 73 | 71 |  |  |  |  |  |  | 1 | 1 |  | 3 |
| **Итого** | **466** | **270** | **142** | **7** | **15** | **2** | **28** | **** | **1** | **1** | **** | **70** |

## libSceAjm (11)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceAjmBatchErrorDump` | `WfAiBW8Wcek` | STUBBED | 0 | `src\core\libraries\ajm\ajm.cpp:52` |
| `sceAjmBatchJobControlBufferRa` | `dmDybN--Fn8` | IMPLEMENTED | 0 | `src\core\libraries\ajm\ajm.cpp:57` |
| `sceAjmBatchJobRunSplitBufferRa` | `7jdAXK+2fMo` | IMPLEMENTED | 0 | `src\core\libraries\ajm\ajm.cpp:83` |
| `sceAjmBatchStartBuffer` | `fFFkk0xfGWs` | IMPLEMENTED | 0 | `src\core\libraries\ajm\ajm.cpp:94` |
| `sceAjmBatchWait` | `-qLsfDAywIY` | IMPLEMENTED | 0 | `src\core\libraries\ajm\ajm.cpp:108` |
| `sceAjmFinalize` | `MHur6qCsUus` | STUBBED | 0 | `src\core\libraries\ajm\ajm.cpp:131` |
| `sceAjmInitialize` | `dl+4eHSzUu4` | IMPLEMENTED | 1 | `src\core\libraries\ajm\ajm.cpp:136` |
| `sceAjmInstanceCreate` | `AxoDrINp4J8` | IMPLEMENTED | 0 | `src\core\libraries\ajm\ajm.cpp:151` |
| `sceAjmInstanceDestroy` | `RbLbuKv8zho` | IMPLEMENTED | 0 | `src\core\libraries\ajm\ajm.cpp:164` |
| `sceAjmModuleRegister` | `Q3dyFuwGn64` | IMPLEMENTED | 1 | `src\core\libraries\ajm\ajm.cpp:197` |
| `sceAjmModuleUnregister` | `Wi7DtlLV+KI` | STUBBED | 0 | `src\core\libraries\ajm\ajm.cpp:211` |

## libSceAppContentUtil (2)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceAppContentGetAddcontInfo` | `m47juOmH0VE` | IMPLEMENTED | 2 | `src\core\libraries\app_content\app_content.cpp:219` |
| `sceAppContentInitialize` | `R9lA82OraNs` | PARTIAL | 1 | `src\core\libraries\app_content\app_content.cpp:300` |

## libSceAudioOut (6)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceAudioOutClose` | `s1--uE9mBFw` | IMPLEMENTED | 0 | `src\core\libraries\audio\audioout.cpp:367` |
| `sceAudioOutInit` | `JfEPXVxhFqA` | IMPLEMENTED | 1 | `src\core\libraries\audio\audioout.cpp:211` |
| `sceAudioOutOpen` | `ekNvsT22rsY` | IMPLEMENTED | 6 | `src\core\libraries\audio\audioout.cpp:229` |
| `sceAudioOutOutput` | `QOQtbeDqsT4` | PARTIAL | 0 | `src\core\libraries\audio\audioout.cpp:534` |
| `sceAudioOutOutputs` | `w3PdaSTSwGE` | PARTIAL | 0 | `src\core\libraries\audio\audioout.cpp:592` |
| `sceAudioOutSetVolume` | `b+uAV89IlxE` | IMPLEMENTED | 0 | `src\core\libraries\audio\audioout.cpp:697` |

## libSceCommonDialog (1)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceCommonDialogInitialize` | `uoUpLGNkygk` | IMPLEMENTED | 1 | `src\core\libraries\system\commondialog.cpp:89` |

## libSceErrorDialog (4)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceErrorDialogInitialize` | `I88KChlynSs` | IMPLEMENTED | 0 | `src\core\libraries\ime\error_dialog.cpp:139` |
| `sceErrorDialogOpen` | `M2ZF-ClLhgY` | IMPLEMENTED | 0 | `src\core\libraries\ime\error_dialog.cpp:148` |
| `sceErrorDialogTerminate` | `9XAxK2PMwk8` | IMPLEMENTED | 0 | `src\core\libraries\ime\error_dialog.cpp:177` |
| `sceErrorDialogUpdateStatus` | `WWiGuh9XfgQ` | IMPLEMENTED | 0 | `src\core\libraries\ime\error_dialog.cpp:189` |

## libSceFios2 (41)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceFiosArchiveGetMountBufferSize` | `ERmiOK9VT0g` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosArchiveMount` | `pIU8u6VsLM8` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosArchiveUnmount` | `YfTBBU5nONQ` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosDHCloseSync` | `0-p4O8FINmU` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosDHOpen` | `uCkgJOrYUL4` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosDHOpenSync` | `GGqucL9F+YI` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosDHReadSync` | `odjOGg8harg` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosDelete` | `nomcox0J32k` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosDeleteSync` | `KsVTc04kN9k` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosDirectoryCreate` | `-ULUBK21QgE` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosDirectoryCreateSync` | `nWuza0ZdfqA` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosDirectoryExistsSync` | `OOuvHKTu4Oc` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosExistsSync` | `2ZaHWy3IhKQ` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHClose` | `5sYNBNK+W3g` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHCloseSync` | `AOujSGqU+ms` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHGetSize` | `FdjoqFQOlt0` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHOpen` | `er6TkQFUvp0` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHOpenSync` | `b44anV2D7K0` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHPread` | `rR8wq7YFRZs` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHPwrite` | `PbxGVfOvUQY` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHReadSync` | `Bn2ZF4ZjeuQ` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHSeek` | `xReSebwKApA` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHSyncSync` | `EzzSJz6yuMc` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHTell` | `MrRFrdgpsx8` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFHWriteSync` | `Kl-TbrDU9YM` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFileDeleteSync` | `bDupEgbQ6Fk` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosFileExistsSync` | `NwOHMRM2Ppw` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosIOFilterAdd` | `lgITuBsRo2o` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosIOFilterPsarcDearchiver` | `OIGbkgGOu6E` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosIOFilterRemove` | `ahIXyuwF0-o` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosInitialize` | `wAKZ-det+yo` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosOpCancel` | `FA7dUleeGik` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosOpDelete` | `5cyEcilO-J0` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosOpGetActualCount` | `+FRvKknUj1I` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosOpWait` | `SnoQQWnGK9I` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosRenameSync` | `G-39lsdSgXo` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosStat` | `QKsI9N7K1zE` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosStatSync` | `jayvY07C5dk` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosTerminate` | `3HAgZPl1v+4` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosTimeGetCurrent` | `NUkBGOZARi4` | LLE | 0 | `LLE:libSceFios2.prx` |
| `sceFiosTimeIntervalFromNanoseconds` | `F1dCP7qkqok` | LLE | 0 | `LLE:libSceFios2.prx` |

## libSceGnmDriver (85)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceGnmAddEqEvent` | `b0xyllnVY-I` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:127` |
| `sceGnmAreSubmitsAllowed` | `b08AgtPlHPg` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:163` |
| `sceGnmBeginWorkload` | `ihxrbsoSKWc` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:168` |
| `sceGnmCreateWorkloadStream` | `5udAm+6boVg` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:207` |
| `sceGnmDebugHardwareStatus` | `qpGITzPE+Zc` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:269` |
| `sceGnmDeleteEqEvent` | `PVT+fuoS9gU` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:274` |
| `sceGnmDestroyWorkloadStream` | `UtObDRQiGbs` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:288` |
| `sceGnmDingDong` | `bX5IbRvECXk` | IMPLEMENTED | 4940 | `src\core\libraries\gnmdriver\gnmdriver.cpp:293` |
| `sceGnmDingDongForWorkload` | `byXlqupd8cE` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:366` |
| `sceGnmDisableMipStatsReport` | `HHo1BAljZO8` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:371` |
| `sceGnmDispatchDirect` | `0BzLGljcwBo` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:376` |
| `sceGnmDispatchIndirect` | `Z43vKp5k7r0` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:393` |
| `sceGnmDispatchIndirectOnMec` | `wED4ZXCFJT0` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:409` |
| `sceGnmDispatchInitDefaultHardwareState` | `nF6bFRUBRAU` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:421` |
| `sceGnmDrawIndex` | `HlTPoZ-oY7Y` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:455` |
| `sceGnmDrawIndexAuto` | `GGsn7jMTxw4` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:476` |
| `sceGnmDrawIndexIndirect` | `ED9-Fjr8Ta4` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:490` |
| `sceGnmDrawIndexIndirectCountMulti` | `thbPcG7E7qk` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:516` |
| `sceGnmDrawIndexIndirectMulti` | `5q95ravnueg` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:553` |
| `sceGnmDrawIndexMultiInstanced` | `jHdPvIzlpKc` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:582` |
| `sceGnmDrawIndexOffset` | `oYM+YzfCm2Y` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:588` |
| `sceGnmDrawIndirect` | `4v+otIIdjqg` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:605` |
| `sceGnmDrawIndirectCountMulti` | `cUCo8OvArrw` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:630` |
| `sceGnmDrawIndirectMulti` | `f5QQLp9rzGk` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:636` |
| `sceGnmDrawInitDefaultHardwareState350` | `yb2cRhagD1I` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:741` |
| `sceGnmDrawInitToDefaultContextState400` | `im2ZuItabu4` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:793` |
| `sceGnmDrawOpaqueAuto` | `stDSYW2SBVM` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:814` |
| `sceGnmDriverCaptureInProgress` | `TLV4mswiZ4A` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:819` |
| `sceGnmDriverTraceInProgress` | `R6z1xM3pW-w` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:873` |
| `sceGnmDriverTriggerCapture` | `d88anrgNoKY` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:879` |
| `sceGnmEndWorkload` | `Fa3x75OOLRA` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:885` |
| `sceGnmFlushGarlic` | `iBt3Oe00Kvc` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:898` |
| `sceGnmGetEqEventType` | `UoYY0DWMC0U` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:928` |
| `sceGnmGetEqTimeStamp` | `H7-fgvEutM0` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:933` |
| `sceGnmGetGpuBlockStatus` | `oL4hGI1PMpw` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:938` |
| `sceGnmGetGpuInfoStatus` | `tZCSL5ulnB4` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:950` |
| `sceGnmGetLastWaitedAddress` | `iFirFzgYsvw` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:956` |
| `sceGnmGetNumTcaUnits` | `KnldROUkWJY` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:962` |
| `sceGnmGetOffChipTessellationBufferSize` | `FFVZcCu3zWU` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:967` |
| `sceGnmGetProtectionFaultTimeStamp` | `fzJdEihTFV4` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:984` |
| `sceGnmGetShaderProgramBaseAddress` | `nEyFbYUloIM` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1020` |
| `sceGnmGetShaderStatus` | `k7iGTvDQPLQ` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1026` |
| `sceGnmGetTheTessellationFactorRingBufferBaseAddress` | `ln33zjBrfjk` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1032` |
| `sceGnmInsertDingDongMarker` | `NfvOrNzy6sk` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1054` |
| `sceGnmInsertPopMarker` | `7qZVNgEu+SY` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1064` |
| `sceGnmInsertPushColorMarker` | `aPIZJTXC+cU` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1076` |
| `sceGnmInsertPushMarker` | `W1Etj-jlW7Y` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1099` |
| `sceGnmInsertSetColorMarker` | `aj3L-iaFmyk` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1120` |
| `sceGnmInsertSetMarker` | `jiItzS6+22g` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1125` |
| `sceGnmInsertThreadTraceMarker` | `URDgJcXhQOs` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1146` |
| `sceGnmInsertWaitFlipDone` | `1qXLHIpROPE` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1152` |
| `sceGnmIsUserPaEnabled` | `jg33rEKLfVs` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1179` |
| `sceGnmLogicalCuIndexToPhysicalCuIndex` | `26PM5Mzl8zc` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1185` |
| `sceGnmLogicalCuMaskToPhysicalCuMask` | `RU74kek-N0c` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1191` |
| `sceGnmMapComputeQueue` | `29oKvKXzEZo` | IMPLEMENTED | 1 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1201` |
| `sceGnmMapComputeQueueWithPriority` | `A+uGq+3KFtQ` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1238` |
| `sceGnmRequestFlipAndSubmitDone` | `gObODli-OH8` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1302` |
| `sceGnmRequestFlipAndSubmitDoneForWorkload` | `6YRHhh5mHCs` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1307` |
| `sceGnmRequestMipStatsReportAndReset` | `f85orjx7qts` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1312` |
| `sceGnmResetVgtControl` | `MYRtYhojKdA` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1317` |
| `sceGnmSetCsShaderWithModifier` | `Kx-h-nWQJ8A` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1409` |
| `sceGnmSetEmbeddedPsShader` | `X9Omw9dwv5M` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1442` |
| `sceGnmSetEmbeddedVsShader` | `+AFvOEXrKJk` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1511` |
| `sceGnmSetEsShader` | `FUHG8sQ3R58` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1563` |
| `sceGnmSetGsRingSizes` | `jtkqXpAOY6w` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1594` |
| `sceGnmSetGsShader` | `UJwNuMBcUAk` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1599` |
| `sceGnmSetHsShader` | `VJNjFtqiF5w` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1627` |
| `sceGnmSetLsShader` | `vckdzbQ46SI` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1657` |
| `sceGnmSetPsShader350` | `5uFKckiJYRM` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1727` |
| `sceGnmSetVgtControl` | `cFCp0NX8wf0` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1794` |
| `sceGnmSetVsShader` | `gAhCn6UiU4Y` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1825` |
| `sceGnmSetWaveLimitMultipliers` | `XiyzNZ9J4nQ` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1863` |
| `sceGnmSetupMipStatsReport` | `+xuDhxlWRPg` | STUBBED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:1789` |
| `sceGnmSubmitAndFlipCommandBuffers` | `xbxNatawohc` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2249` |
| `sceGnmSubmitAndFlipCommandBuffersForWorkload` | `Ga6r7H6Y0RI` | IMPLEMENTED | 4938 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2258` |
| `sceGnmSubmitCommandBuffers` | `zwY0YV91TTI` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2348` |
| `sceGnmSubmitCommandBuffersForWorkload` | `jRcI8VcgTz4` | IMPLEMENTED | 1 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2311` |
| `sceGnmSubmitDone` | `yvZ73uQUqrk` | IMPLEMENTED | 4950 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2355` |
| `sceGnmUnmapComputeQueue` | `ArSg-TGinhk` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2370` |
| `sceGnmUpdateGsShader` | `nLM2i2+65hA` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2393` |
| `sceGnmUpdateHsShader` | `GNlx+y7xPdE` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2424` |
| `sceGnmUpdatePsShader350` | `mLVL7N7BVBg` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2500` |
| `sceGnmUpdateVsShader` | `V31V01UiScY` | IMPLEMENTED | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2545` |
| `sceRazorCaptureImmediate` | `u9YKpRRHe-M` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2661` |
| `sceRazorIsLoaded` | `f33OrruQYbM` | TRIVIAL_RETURN | 0 | `src\core\libraries\gnmdriver\gnmdriver.cpp:2673` |

## libSceHttp (18)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceHttpAddRequestHeader` | `EY28T2bkN7k` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:2816` |
| `sceHttpCreateConnectionWithURL` | `qgxDBjorUxs` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:1358` |
| `sceHttpCreateRequestWithURL` | `Aeu5wVKkF9w` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:1472` |
| `sceHttpCreateTemplate` | `0gYjPTR-6cY` | IMPLEMENTED | 2 | `src\core\libraries\network\http.cpp:1509` |
| `sceHttpDeleteConnection` | `P6A3ytpsiYc` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:3023` |
| `sceHttpDeleteRequest` | `qe7oZ+v4PWA` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:3057` |
| `sceHttpDeleteTemplate` | `4I8vEpuEhZ8` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:3040` |
| `sceHttpGetAllResponseHeaders` | `aCYPMSUIaP8` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:2681` |
| `sceHttpGetStatusCode` | `0a2TBNfE3BU` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:2754` |
| `sceHttpInit` | `A9cVMUtEp4Y` | IMPLEMENTED | 2 | `src\core\libraries\network\http.cpp:1622` |
| `sceHttpReadData` | `P5pdoykPYTk` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:2467` |
| `sceHttpSendRequest` | `1e2BNwI-XzE` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:1666` |
| `sceHttpSetChunkedTransferEnabled` | `PDxS48xGQLs` | STUBBED | 0 | `src\core\libraries\network\http.cpp:1829` |
| `sceHttpSetConnectTimeOut` | `0S9tTH0uqTU` | IMPLEMENTED | 1 | `src\core\libraries\network\http.cpp:2969` |
| `sceHttpSetRecvTimeOut` | `yigr4V0-HTM` | IMPLEMENTED | 1 | `src\core\libraries\network\http.cpp:3003` |
| `sceHttpSetSendTimeOut` | `xegFfZKBVlw` | IMPLEMENTED | 1 | `src\core\libraries\network\http.cpp:2986` |
| `sceHttpTerm` | `Ik-KpLTlf7Q` | IMPLEMENTED | 0 | `src\core\libraries\network\http.cpp:2110` |
| `sceHttpsLoadCert` | `DK+GoXCNT04` | IMPLEMENTED | 2 | `src\core\libraries\network\http.cpp:1962` |

## libSceMsgDialog (3)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceMsgDialogInitialize` | `lDqxaY1UbEo` | IMPLEMENTED | 0 | `src\core\libraries\system\msgdialog.cpp:51` |
| `sceMsgDialogOpen` | `b06Hh0DPEaE` | IMPLEMENTED | 0 | `src\core\libraries\system\msgdialog.cpp:68` |
| `sceMsgDialogUpdateStatus` | `6fIC3XKt2k0` | IMPLEMENTED | 0 | `src\core\libraries\system\msgdialog.cpp:147` |

## libSceNet (30)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceNetAccept` | `PIWqhn9oSxc` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:549` |
| `sceNetBind` | `bErx49PgxyY` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:615` |
| `sceNetConnect` | `OXXX4mUk3uk` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1007` |
| `sceNetEpollControl` | `ZVw46bsasAk` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1150` |
| `sceNetEpollCreate` | `SF47kB2MNTo` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1202` |
| `sceNetEpollDestroy` | `Inp1lfL+Jdw` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1214` |
| `sceNetEpollWait` | `drjIbDbA7UQ` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1245` |
| `sceNetErrnoLoc` | `HQOwnfMGipQ` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1258` |
| `sceNetHtons` | `iWQWrwiSt8A` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1760` |
| `sceNetInetNtop` | `9vA2aW+CHuA` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1764` |
| `sceNetInetPton` | `8Kcp5d-q1Uo` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1788` |
| `sceNetInit` | `Nlev7Lg8k3A` | IMPLEMENTED | 1 | `src\core\libraries\net\net.cpp:1822` |
| `sceNetListen` | `kOj1HiAGE54` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1875` |
| `sceNetNtohs` | `Rbvt+5Y2iEw` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1908` |
| `sceNetPoolCreate` | `dgJBaeJnGpo` | IMPLEMENTED | 1 | `src\core\libraries\net\net.cpp:1912` |
| `sceNetPoolDestroy` | `K7RlrTkI-mw` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1932` |
| `sceNetRecv` | `9wO9XrMsNhc` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:1956` |
| `sceNetRecvfrom` | `304ooNZxWDY` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2005` |
| `sceNetResolverCreate` | `C4UgDHHPvdw` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2101` |
| `sceNetResolverDestroy` | `kJlYH5uMAWI` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2127` |
| `sceNetResolverStartAton` | `Apb4YDxKsRI` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2146` |
| `sceNetResolverStartNtoa` | `Nd91WaWmG2w` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2160` |
| `sceNetSend` | `beRjXBn-z+o` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2207` |
| `sceNetSendto` | `gvD1greCu0A` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2303` |
| `sceNetSetsockopt` | `2mKX2Spso7I` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2326` |
| `sceNetShutdown` | `TSM6whtekok` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2647` |
| `sceNetSocket` | `Q4qBuN-c0ZM` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2680` |
| `sceNetSocketAbort` | `zJGf8xjFnQE` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2687` |
| `sceNetSocketClose` | `45ggEzakPJQ` | IMPLEMENTED | 0 | `src\core\libraries\net\net.cpp:2692` |
| `sceNetTerm` | `cTGkc6-TBlI` | TRIVIAL_RETURN | 0 | `src\core\libraries\net\net.cpp:2730` |

## libSceNetCtl (6)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceNetCtlGetInfo` | `obuxdTiwkF8` | IMPLEMENTED | 2 | `src\core\libraries\network\netctl.cpp:163` |
| `sceNetCtlGetState` | `uBPlr0lbuiI` | IMPLEMENTED | 1 | `src\core\libraries\network\netctl.cpp:342` |
| `sceNetCtlInit` | `gky0+oaNM4k` | STUBBED | 1 | `src\core\libraries\network\netctl.cpp:371` |
| `sceNetCtlRegisterCallback` | `UJ+Z7Q+4ck0` | IMPLEMENTED | 0 | `src\core\libraries\network\netctl.cpp:381` |
| `sceNetCtlTerm` | `Z4wwCFiBELQ` | STUBBED | 0 | `src\core\libraries\network\netctl.cpp:424` |
| `sceNetCtlUnregisterCallback` | `Rqm2OnZMCz0` | STUBBED | 0 | `src\core\libraries\network\netctl.cpp:429` |

## libSceNpManager (14)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceNpCheckCallback` | `3Zl8BePTh9Y` | IMPLEMENTED | 4939 | `src\core\libraries\np\np_manager.cpp:964` |
| `sceNpCheckNpAvailabilityA` | `8Z2Jc5GvGDI` | IMPLEMENTED | 0 | `src\core\libraries\np\np_manager.cpp:211` |
| `sceNpCreateAsyncRequest` | `eiqMCt9UshI` | IMPLEMENTED | 0 | `src\core\libraries\np\np_manager.cpp:187` |
| `sceNpDeleteRequest` | `S7QTn72PrDw` | IMPLEMENTED | 0 | `src\core\libraries\np\np_manager.cpp:454` |
| `sceNpGetAccountCountryA` | `JT+t00a3TxA` | IMPLEMENTED | 0 | `src\core\libraries\np\np_manager.cpp:487` |
| `sceNpGetAccountIdA` | `rbknaUjpqWo` | IMPLEMENTED | 0 | `src\core\libraries\np\np_manager.cpp:579` |
| `sceNpGetGamePresenceStatusA` | `oPO9U42YpgI` | IMPLEMENTED | 0 | `src\core\libraries\np\np_manager.cpp:548` |
| `sceNpGetOnlineId` | `XDncXQIJUSk` | IMPLEMENTED | 0 | `src\core\libraries\np\np_manager.cpp:618` |
| `sceNpGetState` | `eQH7nWPcAgc` | IMPLEMENTED | 1 | `src\core\libraries\np\np_manager.cpp:657` |
| `sceNpPollAsync` | `uqcPJLWL08M` | IMPLEMENTED | 0 | `src\core\libraries\np\np_manager.cpp:428` |
| `sceNpRegisterGamePresenceCallbackA` | `KswxLxk4c1Y` | IMPLEMENTED | 0 | `src\core\libraries\np\np_manager.cpp:1128` |
| `sceNpRegisterStateCallbackA` | `qQJfO8HAiaY` | IMPLEMENTED | 1 | `src\core\libraries\np\np_manager.cpp:1007` |
| `sceNpSetContentRestriction` | `A2CQ3kgSopQ` | PARTIAL | 1 | `src\core\libraries\np\np_manager.cpp:748` |
| `sceNpSetNpTitleId` | `Ec63y59l9tw` | STUBBED | 1 | `src\core\libraries\np\np_manager.cpp:765` |

## libSceNpTrophy (7)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceNpTrophyCreateContext` | `XbkjbobZlCY` | IMPLEMENTED | 1 | `src\core\libraries\np\np_trophy.cpp:273` |
| `sceNpTrophyCreateHandle` | `q7U6tEAQf7c` | IMPLEMENTED | 1 | `src\core\libraries\np\np_trophy.cpp:303` |
| `sceNpTrophyDestroyContext` | `E1Wrwd07Lr8` | IMPLEMENTED | 0 | `src\core\libraries\np\np_trophy.cpp:319` |
| `sceNpTrophyDestroyHandle` | `GNcF4oidY0Y` | IMPLEMENTED | 0 | `src\core\libraries\np\np_trophy.cpp:344` |
| `sceNpTrophyGetTrophyUnlockState` | `LHuSmO3SLd8` | IMPLEMENTED | 2 | `src\core\libraries\np\np_trophy.cpp:808` |
| `sceNpTrophyRegisterContext` | `TJCAxto9SEU` | IMPLEMENTED | 2 | `src\core\libraries\np\np_trophy.cpp:871` |
| `sceNpTrophyUnlockTrophy` | `28xmRUFao68` | IMPLEMENTED | 0 | `src\core\libraries\np\np_trophy.cpp:946` |

## libScePad (7)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `scePadClose` | `6ncge5+l5Qs` | IMPLEMENTED | 0 | `src\core\libraries\pad\pad.cpp:46` |
| `scePadGetControllerInformation` | `gjP9-KQzoUk` | IMPLEMENTED | 4 | `src\core\libraries\pad\pad.cpp:135` |
| `scePadInit` | `hv1luiJrqQM` | STUBBED | 1 | `src\core\libraries\pad\pad.cpp:262` |
| `scePadOpen` | `xk0AcarP3V4` | IMPLEMENTED | 2 | `src\core\libraries\pad\pad.cpp:308` |
| `scePadReadState` | `YndgXqQVV7c` | IMPLEMENTED | 0 | `src\core\libraries\pad\pad.cpp:467` |
| `scePadSetLightBar` | `RR4novUEENY` | IMPLEMENTED | 9878 | `src\core\libraries\pad\pad.cpp:566` |
| `scePadSetVibration` | `yFVnOdGxvZY` | IMPLEMENTED | 9877 | `src\core\libraries\pad\pad.cpp:651` |

## libScePlayGo (10)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `scePlayGoClose` | `Uco1I0dlDi8` | IMPLEMENTED | 0 | `src\core\libraries\playgo\playgo.cpp:28` |
| `scePlayGoGetEta` | `v6EZ-YWRdMs` | IMPLEMENTED | 0 | `src\core\libraries\playgo\playgo.cpp:75` |
| `scePlayGoGetLocus` | `uWIYLFkkwqk` | IMPLEMENTED | 2 | `src\core\libraries\playgo\playgo.cpp:138` |
| `scePlayGoGetProgress` | `-RJWNMK3fC8` | IMPLEMENTED | 1 | `src\core\libraries\playgo\playgo.cpp:171` |
| `scePlayGoGetToDoList` | `Nn7zKwnA5q0` | IMPLEMENTED | 1 | `src\core\libraries\playgo\playgo.cpp:210` |
| `scePlayGoInitialize` | `ts6GlZOKRrE` | IMPLEMENTED | 1 | `src\core\libraries\playgo\playgo.cpp:238` |
| `scePlayGoOpen` | `M1Gma1ocrGE` | IMPLEMENTED | 1 | `src\core\libraries\playgo\playgo.cpp:270` |
| `scePlayGoSetLanguageMask` | `LosLlHOpNqQ` | IMPLEMENTED | 1 | `src\core\libraries\playgo\playgo.cpp:346` |
| `scePlayGoSetToDoList` | `gUPGiOQ1tmQ` | IMPLEMENTED | 1 | `src\core\libraries\playgo\playgo.cpp:361` |
| `scePlayGoTerminate` | `MPe0EeBGM-E` | IMPLEMENTED | 0 | `src\core\libraries\playgo\playgo.cpp:380` |

## libSceSaveData (8)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceSaveDataDelete` | `S1GkePI17zQ` | IMPLEMENTED | 0 | `src\core\libraries\save_data\savedata.cpp:728` |
| `sceSaveDataDirNameSearch` | `dyIhnXq-0SM` | IMPLEMENTED | 4 | `src\core\libraries\save_data\savedata.cpp:779` |
| `sceSaveDataInitialize3` | `TywrFKCoLGY` | IMPLEMENTED | 1 | `src\core\libraries\save_data\savedata.cpp:1206` |
| `sceSaveDataMount2` | `0z45PIH+SNI` | IMPLEMENTED | 3 | `src\core\libraries\save_data\savedata.cpp:1277` |
| `sceSaveDataSaveIcon` | `c88Yy54Mx0w` | IMPLEMENTED | 2 | `src\core\libraries\save_data\savedata.cpp:1372` |
| `sceSaveDataSetParam` | `85zul--eGXs` | IMPLEMENTED | 3 | `src\core\libraries\save_data\savedata.cpp:1419` |
| `sceSaveDataTerminate` | `yKDy8S5yLA0` | IMPLEMENTED | 0 | `src\core\libraries\save_data\savedata.cpp:1666` |
| `sceSaveDataUmount` | `BMR4F-Uek3E` | IMPLEMENTED | 3 | `src\core\libraries\save_data\savedata.cpp:1705` |

## libSceSaveDataDialog (5)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceSaveDataDialogGetResult` | `yEiJ-qqr6Cg` | IMPLEMENTED | 0 | `src\core\libraries\save_data\dialog\savedatadialog.cpp:34` |
| `sceSaveDataDialogInitialize` | `s9e3+YpRnzw` | IMPLEMENTED | 0 | `src\core\libraries\save_data\dialog\savedatadialog.cpp:51` |
| `sceSaveDataDialogOpen` | `4tPhsP6FpDI` | IMPLEMENTED | 0 | `src\core\libraries\save_data\dialog\savedatadialog.cpp:72` |
| `sceSaveDataDialogTerminate` | `YuH2FA7azqQ` | IMPLEMENTED | 0 | `src\core\libraries\save_data\dialog\savedatadialog.cpp:123` |
| `sceSaveDataDialogUpdateStatus` | `KK3Bdg1RWK0` | IMPLEMENTED | 0 | `src\core\libraries\save_data\dialog\savedatadialog.cpp:137` |

## libSceSsl (2)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceSslInit` | `hdpVEUDFW3s` | DUMMY | 1 | `src\core\libraries\network\ssl2.cpp:229` |
| `sceSslTerm` | `0K1yQ6Lv-Yc` | STUBBED | 0 | `src\core\libraries\network\ssl2.cpp:286` |

## libSceSysmodule (2)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceSysmoduleLoadModule` | `g8cM39EUZ6o` | IMPLEMENTED | 9 | `src\core\libraries\sysmodule\sysmodule.cpp:73` |
| `sceSysmoduleUnloadModule` | `eR2bZFAAU0Q` | PARTIAL | 0 | `src\core\libraries\sysmodule\sysmodule.cpp:147` |

## libSceSystemService (6)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceSystemServiceGetDisplaySafeAreaInfo` | `1n37q1Bvc5Y` | IMPLEMENTED | 1 | `src\core\libraries\system\systemservice.cpp:1726` |
| `sceSystemServiceGetStatus` | `rPo6tV8D9bM` | IMPLEMENTED | 0 | `src\core\libraries\system\systemservice.cpp:1777` |
| `sceSystemServiceHideSplashScreen` | `Vo5V8KAwCmk` | IMPLEMENTED | 1 | `src\core\libraries\system\systemservice.cpp:1804` |
| `sceSystemServiceParamGetInt` | `fZo48un7LK4` | IMPLEMENTED | 3 | `src\core\libraries\system\systemservice.cpp:1924` |
| `sceSystemServiceParamGetString` | `SsC-m-S9JTA` | STUBBED | 1 | `src\core\libraries\system\systemservice.cpp:1979` |
| `sceSystemServiceReceiveEvent` | `656LMQSrg6U` | IMPLEMENTED | 0 | `src\core\libraries\system\systemservice.cpp:1994` |

## libSceUserService (5)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceUserServiceGetEvent` | `yH17Q6NWtVg` | IMPLEMENTED | 2 | `src\core\libraries\system\userservice.cpp:126` |
| `sceUserServiceGetInitialUser` | `CdWp0oHWGr0` | IMPLEMENTED | 1 | `src\core\libraries\system\userservice.cpp:513` |
| `sceUserServiceGetLoginUserIdList` | `fPhymKNvK-A` | IMPLEMENTED | 4 | `src\core\libraries\system\userservice.cpp:588` |
| `sceUserServiceInitialize` | `j3YMu1MVNNo` | DUMMY | 1 | `src\core\libraries\system\userservice.cpp:1173` |
| `sceUserServiceTerminate` | `bwFjS+bX9mA` | STUBBED | 0 | `src\core\libraries\system\userservice.cpp:2188` |

## libSceVideoOut (13)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceVideoOutAdjustColor_` | `pv9CI5VC+R0` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:394` |
| `sceVideoOutClose` | `uquVH4-Du78` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:319` |
| `sceVideoOutColorSettingsSetGamma_` | `DYhhWbJSeRg` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:386` |
| `sceVideoOutConfigureOutputMode_` | `N1bEoJ4SRw4` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:424` |
| `sceVideoOutGetDeviceCapabilityInfo_` | `kGVLc3htQE8` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:362` |
| `sceVideoOutGetFlipStatus` | `SbU3dwp80lQ` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:244` |
| `sceVideoOutIsFlipPending` | `zgXifHT9ErY` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:147` |
| `sceVideoOutModeSetAny_` | `pjkDsgxli6c` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:419` |
| `sceVideoOutOpen` | `Up36PTk687E` | IMPLEMENTED | 1 | `src\core\libraries\videoout\video_out.cpp:298` |
| `sceVideoOutRegisterBuffers` | `w3BY+tAEiQY` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:125` |
| `sceVideoOutSetBufferAttribute` | `i6-sR91Wt-4` | IMPLEMENTED | 1 | `src\core\libraries\videoout\video_out.cpp:22` |
| `sceVideoOutSetFlipRate` | `CBiu4mCE1DA` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:141` |
| `sceVideoOutUnregisterBuffers` | `N5KDtkIjjJ4` | IMPLEMENTED | 0 | `src\core\libraries\videoout\video_out.cpp:324` |

## libSceVideodec (6)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `sceVideodecCreateDecoder` | `qkgRiwHyheU` | IMPLEMENTED | 2 | `src\core\libraries\videodec\videodec.cpp:52` |
| `sceVideodecDecode` | `q0W5GJMovMs` | IMPLEMENTED | 0 | `src\core\libraries\videodec\videodec.cpp:74` |
| `sceVideodecDeleteDecoder` | `U0kpGF1cl90` | PARTIAL | 1 | `src\core\libraries\videodec\videodec.cpp:97` |
| `sceVideodecFlush` | `jeigLlKdp5I` | IMPLEMENTED | 3 | `src\core\libraries\videodec\videodec.cpp:109` |
| `sceVideodecQueryResourceInfo` | `leCAscipfFY` | IMPLEMENTED | 2 | `src\core\libraries\videodec\videodec.cpp:137` |
| `sceVideodecReset` | `f8AgDv-1X8A` | PARTIAL | 2 | `src\core\libraries\videodec\videodec.cpp:189` |

## libc (101)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `Need_sceLibc` | `P330P3dFF68` | LLE | 0 | `LLE:libc.prx` |
| `_Dbl` | `e+hi-tOrDZU` | LLE | 0 | `LLE:libc.prx` |
| `_FLog` | `jjjRS7l1MPM` | LLE | 0 | `LLE:libc.prx` |
| `_FSin` | `ZtjspkJQ+vw` | LLE | 0 | `LLE:libc.prx` |
| `_FSinh` | `O4L+0oCN9zA` | LLE | 0 | `LLE:libc.prx` |
| `_Flt` | `+SeQg8c1WC0` | LLE | 0 | `LLE:libc.prx` |
| `_Getpctype` | `sUP1hBaouOw` | LLE | 0 | `LLE:libc.prx` |
| `_Getptolower` | `1uJgoVq3bQU` | LLE | 0 | `LLE:libc.prx` |
| `_Getptoupper` | `rcQCUr0EaRU` | LLE | 0 | `LLE:libc.prx` |
| `_Iswctype` | `CyXs2l-1kNA` | LLE | 0 | `LLE:libc.prx` |
| `_Log` | `zXCi78bYrEI` | LLE | 0 | `LLE:libc.prx` |
| `_Sin` | `cCXjU72Z0Ow` | LLE | 0 | `LLE:libc.prx` |
| `_Stderr` | `H8AprKeZtNg` | LLE | 0 | `LLE:libc.prx` |
| `_Stdout` | `2sWzhYqFH4E` | LLE | 0 | `LLE:libc.prx` |
| `_Stod` | `c41UEHVtiEA` | LLE | 0 | `LLE:libc.prx` |
| `_Stoll` | `7pNKcscKrf8` | LLE | 0 | `LLE:libc.prx` |
| `_Stoul` | `zlfEH8FmyUA` | LLE | 0 | `LLE:libc.prx` |
| `_Stoull` | `q+9E0X3aWpU` | LLE | 0 | `LLE:libc.prx` |
| `__cxa_atexit` | `tsvEmnenz48` | LLE | 0 | `LLE:libc.prx` |
| `__cxa_guard_acquire` | `3GPpjQdAMTw` | LLE | 0 | `LLE:libc.prx` |
| `__cxa_guard_release` | `9rAeANT2tyE` | LLE | 0 | `LLE:libc.prx` |
| `__cxa_pure_virtual` | `zr094EQ39Ww` | LLE | 0 | `LLE:libc.prx` |
| `__isfinitef` | `Q8pvJimUWis` | LLE | 0 | `LLE:libc.prx` |
| `_init_env` | `bzQExy189ZI` | LLE | 0 | `LLE:libc.prx` |
| `acosf` | `QI-x0SL8jhw` | LLE | 0 | `LLE:libc.prx` |
| `asinf` | `GZWjF-YIFFk` | LLE | 0 | `LLE:libc.prx` |
| `atan2` | `HUbZmOnT-Dg` | LLE | 0 | `LLE:libc.prx` |
| `atan2f` | `EH-x713A99c` | LLE | 0 | `LLE:libc.prx` |
| `atanf` | `weDug8QD-lE` | LLE | 0 | `LLE:libc.prx` |
| `atexit` | `8G2LB+A3rzg` | LLE | 0 | `LLE:libc.prx` |
| `bsearch` | `NesIgTmfF0Q` | LLE | 0 | `LLE:libc.prx` |
| `catchReturnFromMain` | `XKRegsFpEpk` | LLE | 0 | `LLE:libc.prx` |
| `exit` | `uMei1W9uyNo` | LLE | 0 | `LLE:libc.prx` |
| `exp2f` | `wuAQt-j+p4o` | LLE | 0 | `LLE:libc.prx` |
| `expf` | `8zsu04XNsZ4` | LLE | 0 | `LLE:libc.prx` |
| `fclose` | `uodLYyUip20` | LLE | 0 | `LLE:libc.prx` |
| `feof` | `LxcEU+ICu8U` | LLE | 0 | `LLE:libc.prx` |
| `ferror` | `AHxyhN96dy4` | LLE | 0 | `LLE:libc.prx` |
| `fflush` | `MUjC4lbHrK4` | LLE | 0 | `LLE:libc.prx` |
| `fmodf` | `88Vv-AzHVj8` | LLE | 0 | `LLE:libc.prx` |
| `fopen` | `xeYO4u7uyJ0` | LLE | 0 | `LLE:libc.prx` |
| `fprintf` | `fffwELXNVFA` | LLE | 0 | `LLE:libc.prx` |
| `fputc` | `aZK8lNei-Qw` | LLE | 0 | `LLE:libc.prx` |
| `fputs` | `QrZZdJ8XsX0` | LLE | 0 | `LLE:libc.prx` |
| `fread` | `lbB+UlZqVG0` | LLE | 0 | `LLE:libc.prx` |
| `free` | `tIhsqj0qsFE` | LLE | 0 | `LLE:libc.prx` |
| `frexpf` | `aaDMGGkXFxo` | LLE | 0 | `LLE:libc.prx` |
| `fseek` | `rQFVBXp-Cxg` | LLE | 0 | `LLE:libc.prx` |
| `ftell` | `Qazy8LmXTvw` | LLE | 0 | `LLE:libc.prx` |
| `fwrite` | `MpxhMh8QFro` | LLE | 0 | `LLE:libc.prx` |
| `gmtime_s` | `5bBacGLyLOs` | LLE | 0 | `LLE:libc.prx` |
| `ldexpf` | `kn0yiYeExgA` | LLE | 0 | `LLE:libc.prx` |
| `localeconv` | `0hlfW1O4Aa4` | LLE | 0 | `LLE:libc.prx` |
| `malloc` | `gQX+4GDQjpM` | LLE | 0 | `LLE:libc.prx` |
| `memalign` | `Ujf3KzMvRmI` | LLE | 0 | `LLE:libc.prx` |
| `memcmp` | `DfivPArhucg` | LLE | 0 | `LLE:libc.prx` |
| `memcpy` | `Q3VBxCXhUHs` | LLE | 0 | `LLE:libc.prx` |
| `memmove` | `+P6FRGH4LfA` | LLE | 0 | `LLE:libc.prx` |
| `memset` | `8zTFvBIAIN8` | LLE | 0 | `LLE:libc.prx` |
| `mktime` | `n7AepwR0s34` | LLE | 0 | `LLE:libc.prx` |
| `perror` | `EMutwaQ34Jo` | LLE | 0 | `LLE:libc.prx` |
| `pow` | `9LCjpWyQ5Zc` | LLE | 0 | `LLE:libc.prx` |
| `powf` | `1D0H2KNjshE` | LLE | 0 | `LLE:libc.prx` |
| `printf` | `hcuQgD53UxM` | LLE | 0 | `LLE:libc.prx` |
| `puts` | `YQ0navp+YIc` | LLE | 0 | `LLE:libc.prx` |
| `qsort` | `AEJdIVZTEmo` | LLE | 0 | `LLE:libc.prx` |
| `rand` | `cpCOXWMgha0` | LLE | 0 | `LLE:libc.prx` |
| `sceLibcMspaceCreate` | `-hn1tcVHq5Q` | LLE | 0 | `LLE:libc.prx` |
| `sceLibcMspaceDestroy` | `W6SiVSiCDtI` | LLE | 0 | `LLE:libc.prx` |
| `sceLibcMspaceFree` | `Vla-Z+eXlxo` | LLE | 0 | `LLE:libc.prx` |
| `sceLibcMspaceMallocStats` | `mfHdJTIvhuo` | LLE | 0 | `LLE:libc.prx` |
| `sceLibcMspaceMallocStatsFast` | `k04jLXu3+Ic` | LLE | 0 | `LLE:libc.prx` |
| `sceLibcMspaceMallocUsableSize` | `fEoW6BJsPt4` | LLE | 0 | `LLE:libc.prx` |
| `sceLibcMspaceMemalign` | `iF1iQHzxBJU` | LLE | 0 | `LLE:libc.prx` |
| `sceLibcMspaceReallocalign` | `p6lrRW8-MLY` | LLE | 0 | `LLE:libc.prx` |
| `snprintf` | `eLdDw6l0-bU` | LLE | 0 | `LLE:libc.prx` |
| `sprintf` | `tcVi5SivF7Q` | LLE | 0 | `LLE:libc.prx` |
| `srand` | `VPbJwTCgME0` | LLE | 0 | `LLE:libc.prx` |
| `sscanf` | `1Pk0qZQGeWo` | LLE | 0 | `LLE:libc.prx` |
| `strcasecmp` | `AV6ipCNa4Rw` | LLE | 0 | `LLE:libc.prx` |
| `strcat` | `Ls4tzzhimqQ` | LLE | 0 | `LLE:libc.prx` |
| `strchr` | `ob5xAW4ln-0` | LLE | 0 | `LLE:libc.prx` |
| `strcmp` | `Ovb2dSJOAuE` | LLE | 0 | `LLE:libc.prx` |
| `strcpy` | `kiZSXIWd9vg` | LLE | 0 | `LLE:libc.prx` |
| `strcspn` | `q0F6yS-rCms` | LLE | 0 | `LLE:libc.prx` |
| `strftime` | `Av3zjWi64Kw` | LLE | 0 | `LLE:libc.prx` |
| `strlen` | `j4ViWNHEgww` | LLE | 0 | `LLE:libc.prx` |
| `strncasecmp` | `pXvbDfchu6k` | LLE | 0 | `LLE:libc.prx` |
| `strncat` | `kHg45qPC6f0` | LLE | 0 | `LLE:libc.prx` |
| `strncmp` | `aesyjrHVWy4` | LLE | 0 | `LLE:libc.prx` |
| `strncpy` | `6sJWiWSRuqk` | LLE | 0 | `LLE:libc.prx` |
| `strrchr` | `9yDWMxEFdJU` | LLE | 0 | `LLE:libc.prx` |
| `strstr` | `viiwFMaNamA` | LLE | 0 | `LLE:libc.prx` |
| `strtod` | `2vDqwBlpF-o` | LLE | 0 | `LLE:libc.prx` |
| `tanf` | `ZE6RNL+eLbk` | LLE | 0 | `LLE:libc.prx` |
| `time` | `wLlFkwG9UcQ` | LLE | 0 | `LLE:libc.prx` |
| `vfprintf` | `pDBDcY6uLSA` | LLE | 0 | `LLE:libc.prx` |
| `vsnprintf` | `Q2V+iqvjgC0` | LLE | 0 | `LLE:libc.prx` |
| `vsprintf` | `jbz9I9vkqkk` | LLE | 0 | `LLE:libc.prx` |
| `wcslen` | `WkkeywLJcgU` | LLE | 0 | `LLE:libc.prx` |
| `wcsrtombs` | `sOOMlZoy1pg` | LLE | 0 | `LLE:libc.prx` |

## libkernel (73)

| Функция | NID | Статус | seen | Реализация shadPS4 |
|---|---|---|---:|---|
| `__error` | `9BcDykPmo1I` | IMPLEMENTED | 0 | `src\core\libraries\kernel\kernel.cpp:88` |
| `__stack_chk_fail` | `Ou3iL1abvng` | NO_BODY_FOUND | 0 | `HLE` |
| `__stack_chk_guard` | `f7uOxY9mM1U` | OBJECT | 0 | `HLE` |
| `__tls_get_addr` | `vNe1w4diLCs` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\tcb.cpp:89` |
| `clock_gettime` | `lLMT9vJAck0` | IMPLEMENTED | 0 | `src\core\libraries\kernel\time.cpp:120` |
| `gettimeofday` | `n88vx3C5nW8` | IMPLEMENTED | 0 | `src\core\libraries\kernel\time.cpp:405` |
| `pthread_attr_init` | `wtkt-teR1so` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:125` |
| `pthread_attr_setdetachstate` | `E+tyo3lp5Lw` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:197` |
| `pthread_create_name_np` | `Jmi+9w9u0E4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:284` |
| `pthread_join` | `h9CcP3J0oVM` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:209` |
| `pthread_mutex_destroy` | `ltCfaGr2JGE` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:110` |
| `pthread_mutex_init` | `ttHNfU+qDBU` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:100` |
| `pthread_mutex_lock` | `7H0iTOciTLo` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:263` |
| `pthread_mutex_trylock` | `K-jXhbt2gn4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:258` |
| `pthread_mutex_unlock` | `2Z+PpY6CaJg` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:304` |
| `pthread_mutexattr_destroy` | `HF7lK46xzjY` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:458` |
| `pthread_mutexattr_init` | `dQHWEsJtoE4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:358` |
| `pthread_mutexattr_settype` | `mDmgMOGVUqg` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:441` |
| `pthread_self` | `EotR8a3ASf4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:411` |
| `sceKernelAddUserEvent` | `4R6-OvI2cEA` | IMPLEMENTED | 0 | `src\core\libraries\kernel\equeue.cpp:574` |
| `sceKernelAllocateDirectMemory` | `rTXw65xmLIA` | IMPLEMENTED | 4 | `src\core\libraries\kernel\memory.cpp:45` |
| `sceKernelCreateEqueue` | `D0OdFMjp46I` | IMPLEMENTED | 1 | `src\core\libraries\kernel\equeue.cpp:417` |
| `sceKernelCreateEventFlag` | `BpFoboUJoZU` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\event_flag.cpp:249` |
| `sceKernelCreateSema` | `188x57JYp0g` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\semaphore.cpp:319` |
| `sceKernelDeleteEqueue` | `jpFjmgAC5AE` | IMPLEMENTED | 0 | `src\core\libraries\kernel\equeue.cpp:450` |
| `sceKernelDeleteEventFlag` | `8mql9OcQnd4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\event_flag.cpp:297` |
| `sceKernelDeleteSema` | `R1Jvn8bSCW8` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\semaphore.cpp:360` |
| `sceKernelDeleteUserEvent` | `LJDwdSNTnDg` | IMPLEMENTED | 0 | `src\core\libraries\kernel\equeue.cpp:626` |
| `sceKernelGetDirectMemorySize` | `pO96TwzOm5E` | IMPLEMENTED | 0 | `src\core\libraries\kernel\memory.cpp:33` |
| `sceKernelGetPrtAperture` | `L0v2Go5jOuM` | IMPLEMENTED | 0 | `src\core\libraries\kernel\memory.cpp:977` |
| `sceKernelGetTscFrequency` | `1j3S3n-tTW4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\time.cpp:36` |
| `sceKernelGettimeofday` | `ejekcaNQNq0` | IMPLEMENTED | 0 | `src\core\libraries\kernel\time.cpp:453` |
| `sceKernelIsNeoMode` | `WslcK1FQcGI` | IMPLEMENTED | 0 | `src\core\libraries\kernel\process.cpp:19` |
| `sceKernelMapDirectMemory` | `L-Q3LEjIbgA` | IMPLEMENTED | 0 | `src\core\libraries\kernel\memory.cpp:259` |
| `sceKernelMprotect` | `vSMAm3cxYTY` | IMPLEMENTED | 0 | `src\core\libraries\kernel\memory.cpp:398` |
| `sceKernelQueryMemoryProtection` | `WFcfL2lzido` | IMPLEMENTED | 0 | `src\core\libraries\kernel\memory.cpp:393` |
| `sceKernelReleaseDirectMemory` | `MBuItvba6z8` | IMPLEMENTED | 0 | `src\core\libraries\kernel\memory.cpp:113` |
| `sceKernelSetEventFlag` | `IOnSvHzqu6A` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\event_flag.cpp:335` |
| `sceKernelSignalSema` | `4czppHBiriw` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\semaphore.cpp:336` |
| `sceKernelTriggerUserEvent` | `F6e0kwo4cnk` | IMPLEMENTED | 0 | `src\core\libraries\kernel\equeue.cpp:615` |
| `sceKernelUsleep` | `1jfXLRVzisc` | IMPLEMENTED | 0 | `src\core\libraries\kernel\time.cpp:96` |
| `sceKernelWaitEqueue` | `fzyMKs9kim0` | IMPLEMENTED | 0 | `src\core\libraries\kernel\equeue.cpp:462` |
| `sceKernelWaitEventFlag` | `JTvBflhYazQ` | IMPLEMENTED | 27 | `src\core\libraries\kernel\threads\event_flag.cpp:391` |
| `sceKernelWaitSema` | `Zxa0VhQVTsk` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\semaphore.cpp:329` |
| `scePthreadAttrDestroy` | `62KCwEMmzcM` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:39` |
| `scePthreadAttrGetschedparam` | `FXPWHNk8Of0` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:72` |
| `scePthreadAttrInit` | `nsYoNRywwNg` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:125` |
| `scePthreadAttrSetaffinity` | `3qxgM4ezETA` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:317` |
| `scePthreadAttrSetdetachstate` | `-Wreprtu0Qs` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:197` |
| `scePthreadAttrSetinheritsched` | `eXbUSpEaTsA` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:228` |
| `scePthreadAttrSetschedparam` | `DzES9hQF4f4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:209` |
| `scePthreadAttrSetschedpolicy` | `4+h9EzwKF4I` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:143` |
| `scePthreadAttrSetstacksize` | `UTXzJbWhhTE` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread_attr.cpp:189` |
| `scePthreadCondBroadcast` | `JGgj7Uvrl+A` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\condvar.cpp:346` |
| `scePthreadCondDestroy` | `g+PZd2hiacg` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\condvar.cpp:86` |
| `scePthreadCondInit` | `2Tb92quprl0` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\condvar.cpp:80` |
| `scePthreadCondWait` | `WKAXJ4XBPQ4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\condvar.cpp:201` |
| `scePthreadCreate` | `6UgtwV+0zb4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:284` |
| `scePthreadEqual` | `3PtV6p3QNX4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:407` |
| `scePthreadGetaffinity` | `rcrVFJsQWRY` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:1182` |
| `scePthreadGetprio` | `1tKyG7RlMJo` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:594` |
| `scePthreadJoin` | `onNY9Byn-W8` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:209` |
| `scePthreadMutexDestroy` | `2Of0f+3mhhE` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:110` |
| `scePthreadMutexInit` | `cmo1RIYva9o` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:105` |
| `scePthreadMutexLock` | `9UK1vLZQft4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:263` |
| `scePthreadMutexUnlock` | `tn3VlD0hG60` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:304` |
| `scePthreadMutexattrDestroy` | `smWEktiyyG0` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:458` |
| `scePthreadMutexattrInit` | `F8bUHwAG284` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:358` |
| `scePthreadMutexattrSettype` | `iMp8QpE+XO4` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\mutex.cpp:441` |
| `scePthreadSelf` | `aI+OeCz8xrQ` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:411` |
| `scePthreadSetaffinity` | `bt3CTBKmGyI` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:1191` |
| `scePthreadSetprio` | `W0Hpm2X0uPE` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:606` |
| `scePthreadYield` | `T72hz6ffq08` | IMPLEMENTED | 0 | `src\core\libraries\kernel\threads\pthread.cpp:415` |

## Импорты LLE-модулей из sce_module

| Модуль | Функция | Источник | Статус | Реализация |
|---|---|---|---|---|
| libc.prx | `sceFiosDeallocatePassthruFH` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceFiosDeleteSync` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceFiosFHCloseSync` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceFiosFHOpenWithModeSync` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceFiosFHReadSync` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceFiosFHSeek` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceFiosFHToFileno` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceFiosFHWriteSync` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceFiosFilenoToFH` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceFiosIsValidHandle` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceFiosRenameSync` | libSceFios2/libSceFios2 | LLE | `LLE:libSceFios2.prx` |
| libc.prx | `sceLibcHeapGetTraceInfo` | libSceLibcInternal/libSceLibcInternalExt | IMPLEMENTED | `src\core\libraries\libc_internal\libc_internal_memory.cpp:44` |
| libc.prx | `__elf_phdr_match_addr` | libkernel/libkernel | MISSING | `COMMON_STUB` |
| libc.prx | `__error` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\kernel.cpp:88` |
| libc.prx | `__progname` | libkernel/libkernel | OBJECT | `HLE` |
| libc.prx | `__pthread_cxa_finalize` | libkernel/libkernel | MISSING | `COMMON_STUB` |
| libc.prx | `__stack_chk_fail` | libkernel/libkernel | NO_BODY_FOUND | `HLE` |
| libc.prx | `__stack_chk_guard` | libkernel/libkernel | OBJECT | `HLE` |
| libc.prx | `__tls_get_addr` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\tcb.cpp:89` |
| libc.prx | `_exit` | libkernel/libkernel | UNIMPLEMENTED | `src\core\libraries\kernel\process.cpp:295` |
| libc.prx | `_is_signal_return` | libkernel/libkernel | MISSING | `COMMON_STUB` |
| libc.prx | `_read` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:533` |
| libc.prx | `_sceKernelRtldSetApplicationHeapAPI` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\memory.cpp:468` |
| libc.prx | `_sigprocmask` | libkernel/libkernel | TRIVIAL_RETURN | `src\core\libraries\kernel\kernel.cpp:213` |
| libc.prx | `_write` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:313` |
| libc.prx | `clock_gettime` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\time.cpp:120` |
| libc.prx | `getpagesize` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\kernel.cpp:217` |
| libc.prx | `getrusage` | libkernel/libkernel | MISSING | `COMMON_STUB` |
| libc.prx | `gettimeofday` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\time.cpp:405` |
| libc.prx | `mmap` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\memory.cpp:753` |
| libc.prx | `msync` | libkernel/libkernel | STUBBED | `src\core\libraries\kernel\memory.cpp:942` |
| libc.prx | `nanosleep` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\time.cpp:77` |
| libc.prx | `pthread_mutex_lock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:263` |
| libc.prx | `pthread_mutex_unlock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:304` |
| libc.prx | `sceKernelConvertLocaltimeToUtc` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\time.cpp:467` |
| libc.prx | `sceKernelConvertUtcToLocaltime` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\time.cpp:496` |
| libc.prx | `sceKernelDebugRaiseException` | libkernel/libkernel | UNIMPLEMENTED | `src\core\libraries\kernel\threads\exception.cpp:626` |
| libc.prx | `sceKernelDebugRaiseExceptionOnReleaseMode` | libkernel/libkernel | UNIMPLEMENTED | `src\core\libraries\kernel\threads\exception.cpp:634` |
| libc.prx | `sceKernelGetModuleInfoForUnwind` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\process.cpp:136` |
| libc.prx | `sceKernelGetModuleInfoFromAddr` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\process.cpp:169` |
| libc.prx | `sceKernelGetProcParam` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\process.cpp:83` |
| libc.prx | `sceKernelGetProcessTime` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\time.cpp:40` |
| libc.prx | `sceKernelInternalMemoryGetModuleSegmentInfo` | libkernel/libkernel | MISSING | `COMMON_STUB` |
| libc.prx | `sceKernelLseek` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:524` |
| libc.prx | `sceKernelMapNamedFlexibleMemory` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\memory.cpp:313` |
| libc.prx | `sceKernelMlock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\memory.cpp:877` |
| libc.prx | `sceKernelMunmap` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\memory.cpp:842` |
| libc.prx | `sceKernelPrintBacktraceWithModuleInfo` | libkernel/libkernel | MISSING | `COMMON_STUB` |
| libc.prx | `scePthreadAttrDestroy` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:39` |
| libc.prx | `scePthreadAttrGet` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:248` |
| libc.prx | `scePthreadAttrGetaffinity` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:308` |
| libc.prx | `scePthreadAttrInit` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:125` |
| libc.prx | `scePthreadAttrSetstacksize` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:189` |
| libc.prx | `scePthreadCondBroadcast` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:346` |
| libc.prx | `scePthreadCondDestroy` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:86` |
| libc.prx | `scePthreadCondInit` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:80` |
| libc.prx | `scePthreadCondSignal` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:334` |
| libc.prx | `scePthreadCondTimedwait` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:219` |
| libc.prx | `scePthreadCondWait` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:201` |
| libc.prx | `scePthreadCreate` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:284` |
| libc.prx | `scePthreadDetach` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:223` |
| libc.prx | `scePthreadEqual` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:407` |
| libc.prx | `scePthreadExit` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:84` |
| libc.prx | `scePthreadGetspecific` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_spec.cpp:128` |
| libc.prx | `scePthreadJoin` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:209` |
| libc.prx | `scePthreadKeyCreate` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_spec.cpp:17` |
| libc.prx | `scePthreadKeyDelete` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_spec.cpp:31` |
| libc.prx | `scePthreadMutexDestroy` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:110` |
| libc.prx | `scePthreadMutexInit` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:105` |
| libc.prx | `scePthreadMutexLock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:263` |
| libc.prx | `scePthreadMutexTrylock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:258` |
| libc.prx | `scePthreadMutexUnlock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:304` |
| libc.prx | `scePthreadMutexattrDestroy` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:458` |
| libc.prx | `scePthreadMutexattrInit` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:358` |
| libc.prx | `scePthreadMutexattrSettype` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:441` |
| libc.prx | `scePthreadOnce` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:427` |
| libc.prx | `scePthreadSelf` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:411` |
| libc.prx | `scePthreadSetspecific` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_spec.cpp:100` |
| libc.prx | `scePthreadYield` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:415` |
| libc.prx | `sched_yield` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:419` |
| libc.prx | `sigfillset` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\exception.cpp:308` |
| libc.prx | `signal` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\exception.cpp:526` |
| libc.prx | `sigprocmask` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\exception.cpp:457` |
| libc.prx | `sigreturn` | libkernel/libkernel | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `?` | libSceDiscMap/libSceDiscMap | IMPLEMENTED | `src\core\libraries\disc_map\disc_map.cpp:20` |
| libSceFios2.prx | `sceDiscMapIsRequestOnHDD` | libSceDiscMap/libSceDiscMap | TRIVIAL_RETURN | `src\core\libraries\disc_map\disc_map.cpp:16` |
| libSceFios2.prx | `Need_sceLibcInternal` | libSceLibcInternal/libSceLibcInternal | MISSING | `UNRESOLVED_OBJECT` |
| libSceFios2.prx | `_Stdout` | libSceLibcInternal/libSceLibcInternal | MISSING | `UNRESOLVED_OBJECT` |
| libSceFios2.prx | `__cxa_finalize` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `fflush` | libSceLibcInternal/libSceLibcInternal | IMPLEMENTED | `src\core\libraries\libc_internal\libc_internal_io.cpp:210` |
| libSceFios2.prx | `free` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `gmtime` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `gmtime_s` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `localtime` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `malloc` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `memcpy` | libSceLibcInternal/libSceLibcInternal | IMPLEMENTED | `src\core\libraries\libc_internal\libc_internal_memory.cpp:16` |
| libSceFios2.prx | `memset` | libSceLibcInternal/libSceLibcInternal | IMPLEMENTED | `src\core\libraries\libc_internal\libc_internal_memory.cpp:12` |
| libSceFios2.prx | `mktime` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `printf` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `time` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `vprintf` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `vsnprintf` | libSceLibcInternal/libSceLibcInternal | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `sceSysmoduleLoadModule` | libSceSysmodule/libSceSysmodule | IMPLEMENTED | `src\core\libraries\sysmodule\sysmodule.cpp:73` |
| libSceFios2.prx | `sceSysmoduleUnloadModule` | libSceSysmodule/libSceSysmodule | PARTIAL | `src\core\libraries\sysmodule\sysmodule.cpp:147` |
| libSceFios2.prx | `__error` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\kernel.cpp:88` |
| libSceFios2.prx | `__stack_chk_fail` | libkernel/libkernel | NO_BODY_FOUND | `HLE` |
| libSceFios2.prx | `__stack_chk_guard` | libkernel/libkernel | OBJECT | `HLE` |
| libSceFios2.prx | `__tls_get_addr` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\tcb.cpp:89` |
| libSceFios2.prx | `sceKernelClockGettime` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\time.cpp:297` |
| libSceFios2.prx | `sceKernelClose` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:304` |
| libSceFios2.prx | `sceKernelFstat` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:865` |
| libSceFios2.prx | `sceKernelFsync` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:1077` |
| libSceFios2.prx | `sceKernelFtruncate` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:905` |
| libSceFios2.prx | `sceKernelGetdirentries` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:1151` |
| libSceFios2.prx | `sceKernelGettimeofday` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\time.cpp:453` |
| libSceFios2.prx | `sceKernelLseek` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:524` |
| libSceFios2.prx | `sceKernelMkdir` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:621` |
| libSceFios2.prx | `sceKernelOpen` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:268` |
| libSceFios2.prx | `sceKernelPread` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:1052` |
| libSceFios2.prx | `sceKernelPwrite` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:1207` |
| libSceFios2.prx | `sceKernelRead` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:573` |
| libSceFios2.prx | `sceKernelRename` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:979` |
| libSceFios2.prx | `sceKernelRmdir` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:665` |
| libSceFios2.prx | `sceKernelStat` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:771` |
| libSceFios2.prx | `sceKernelTruncate` | libkernel/libkernel | MISSING | `COMMON_STUB` |
| libSceFios2.prx | `sceKernelUnlink` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:1268` |
| libSceFios2.prx | `sceKernelUsleep` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\time.cpp:96` |
| libSceFios2.prx | `sceKernelWrite` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\file_system.cpp:344` |
| libSceFios2.prx | `scePthreadAttrDestroy` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:39` |
| libSceFios2.prx | `scePthreadAttrInit` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:125` |
| libSceFios2.prx | `scePthreadAttrSetaffinity` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:317` |
| libSceFios2.prx | `scePthreadAttrSetdetachstate` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:197` |
| libSceFios2.prx | `scePthreadAttrSetinheritsched` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:228` |
| libSceFios2.prx | `scePthreadAttrSetschedparam` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:209` |
| libSceFios2.prx | `scePthreadAttrSetstacksize` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread_attr.cpp:189` |
| libSceFios2.prx | `scePthreadCondBroadcast` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:346` |
| libSceFios2.prx | `scePthreadCondDestroy` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:86` |
| libSceFios2.prx | `scePthreadCondInit` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:80` |
| libSceFios2.prx | `scePthreadCondSignal` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:334` |
| libSceFios2.prx | `scePthreadCondTimedwait` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:219` |
| libSceFios2.prx | `scePthreadCondWait` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\condvar.cpp:201` |
| libSceFios2.prx | `scePthreadCreate` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:284` |
| libSceFios2.prx | `scePthreadEqual` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:407` |
| libSceFios2.prx | `scePthreadGetaffinity` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:1182` |
| libSceFios2.prx | `scePthreadGetprio` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:594` |
| libSceFios2.prx | `scePthreadJoin` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:209` |
| libSceFios2.prx | `scePthreadMutexDestroy` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:110` |
| libSceFios2.prx | `scePthreadMutexInit` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:105` |
| libSceFios2.prx | `scePthreadMutexLock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:263` |
| libSceFios2.prx | `scePthreadMutexTrylock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:258` |
| libSceFios2.prx | `scePthreadMutexUnlock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:304` |
| libSceFios2.prx | `scePthreadMutexattrDestroy` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:458` |
| libSceFios2.prx | `scePthreadMutexattrInit` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:358` |
| libSceFios2.prx | `scePthreadMutexattrSetprotocol` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:404` |
| libSceFios2.prx | `scePthreadMutexattrSettype` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\mutex.cpp:441` |
| libSceFios2.prx | `scePthreadRwlockDestroy` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\rwlock.cpp:49` |
| libSceFios2.prx | `scePthreadRwlockInit` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\rwlock.cpp:74` |
| libSceFios2.prx | `scePthreadRwlockRdlock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\rwlock.cpp:136` |
| libSceFios2.prx | `scePthreadRwlockTryrdlock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\rwlock.cpp:171` |
| libSceFios2.prx | `scePthreadRwlockTrywrlock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\rwlock.cpp:184` |
| libSceFios2.prx | `scePthreadRwlockUnlock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\rwlock.cpp:209` |
| libSceFios2.prx | `scePthreadRwlockWrlock` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\rwlock.cpp:196` |
| libSceFios2.prx | `scePthreadSelf` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:411` |
| libSceFios2.prx | `scePthreadSetaffinity` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:1191` |
| libSceFios2.prx | `scePthreadSetprio` | libkernel/libkernel | IMPLEMENTED | `src\core\libraries\kernel\threads\pthread.cpp:606` |
