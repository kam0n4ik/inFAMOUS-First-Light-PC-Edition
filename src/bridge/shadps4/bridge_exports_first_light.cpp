// SPDX-License-Identifier: GPL-2.0-or-later
// SecondSonPC -- TEMPORARY Phase 1-4 bridge: the shadPS4 HLE functions for the imports inFAMOUS: First Light has and
// Second Son has not (loader/generated/import_table_cusa00897.inc, rows "Phase0::Hle" that the generated table does
// not serve): leaderboards (libSceNpScore), title user storage (libSceNpTus), the NP Web API and four libSceRtc clock
// functions. They behave as shadPS4 does with no PSN account signed in to its NP handler: a title context or a Web API
// context is refused with ORBIS_NP_ERROR_SIGNED_OUT, so the game sees these online services unavailable - the PC
// Edition has no PlayStation Network. The clock functions are complete. Same layout as generated/bridge_exports.cpp.

#include <cstdint>
#include <iterator>
#include <vector>

#include "bridge/shadps4/bridge_exports.h"
#include "core/tls.h" // HOST_CALL()
#include "core/libraries/np/np_score/np_score.h"
#include "core/libraries/np/np_tus/np_tus.h"
#include "core/libraries/np/np_web_api/np_web_api.h"
#include "core/libraries/rtc/rtc.h"

// Defined in np_web_api.cpp without a declaration in its header.
namespace Libraries::Np::NpWebApi {
s32 PS4_SYSV_ABI sceNpWebApiGetHttpResponseHeaderValue(s64 requestId, const char* pFieldName, char* pValue,
                                                       u64 valueSize);
s32 PS4_SYSV_ABI sceNpWebApiGetHttpResponseHeaderValueLength(s64 requestId, const char* pFieldName,
                                                             u64* pValueLength);
s32 PS4_SYSV_ABI sceNpWebApiReadData(s64 requestId, void* pData, u64 size);
} // namespace Libraries::Np::NpWebApi

namespace SecondSon::Bridge {

namespace {

template <typename T>
constexpr const void* Addr(T* p) {
    return reinterpret_cast<const void*>(p);
}

namespace Score = Libraries::Np::NpScore;
namespace Tus = Libraries::Np::NpTus;
namespace WebApi = Libraries::Np::NpWebApi;
namespace Rtc = Libraries::Rtc;

// clang-format off
const BridgeExport kFirstLightExports[] = {
    {"ANJssPz3mY0", "libSceNpScore", "libSceNpScore", Addr(HOST_CALL(Score::sceNpScoreRecordScoreAsync))},
    {"G0pE+RNCwfk", "libSceNpScore", "libSceNpScore", Addr(HOST_CALL(Score::sceNpScoreDeleteNpTitleCtx))},
    {"GWnWQNXZH5M", "libSceNpScore", "libSceNpScore", Addr(HOST_CALL(Score::sceNpScoreCreateNpTitleCtxA))},
    {"dK8-SgYf6r4", "libSceNpScore", "libSceNpScore", Addr(HOST_CALL(Score::sceNpScoreDeleteRequest))},
    {"dRszNNyGWkw", "libSceNpScore", "libSceNpScore", Addr(HOST_CALL(Score::sceNpScoreGetRankingByAccountIdAsync))},
    {"gW8qyjYrUbk", "libSceNpScore", "libSceNpScore", Addr(HOST_CALL(Score::sceNpScoreCreateRequest))},
    {"m1DfNRstkSQ", "libSceNpScore", "libSceNpScore", Addr(HOST_CALL(Score::sceNpScorePollAsync))},
    {"y5ja7WI05rs", "libSceNpScore", "libSceNpScore", Addr(HOST_CALL(Score::sceNpScoreGetRankingByRangeAAsync))},
    {"1n-dGukBgnY", "libSceNpTus", "libSceNpTus", Addr(HOST_CALL(Tus::sceNpTusCreateNpTitleCtxA))},
    {"2BnPSY1Oxd8", "libSceNpTus", "libSceNpTus", Addr(HOST_CALL(Tus::sceNpTusGetMultiSlotVariableAAsync))},
    {"3bh2aBvvmvM", "libSceNpTus", "libSceNpTus", Addr(HOST_CALL(Tus::sceNpTusCreateRequest))},
    {"CcIH40dYS88", "libSceNpTus", "libSceNpTus", Addr(HOST_CALL(Tus::sceNpTusDeleteRequest))},
    {"H3uq7x0sZOI", "libSceNpTus", "libSceNpTus", Addr(HOST_CALL(Tus::sceNpTusDeleteNpTitleCtx))},
    {"eGunerNP9n0", "libSceNpTus", "libSceNpTus", Addr(HOST_CALL(Tus::sceNpTusGetMultiUserVariableAAsync))},
    {"t7b6dmpQNiI", "libSceNpTus", "libSceNpTus", Addr(HOST_CALL(Tus::sceNpTusPollAsync))},
    {"ypMObSwfcns", "libSceNpTus", "libSceNpTus", Addr(HOST_CALL(Tus::sceNpTusSetMultiSlotVariableAAsync))},
    {"743ZzEBzlV8", "libSceNpWebApi", "libSceNpWebApi", Addr(HOST_CALL(WebApi::sceNpWebApiGetHttpResponseHeaderValueLength))},
    {"CQtPRSF6Ds8", "libSceNpWebApi", "libSceNpWebApi", Addr(HOST_CALL(WebApi::sceNpWebApiReadData))},
    {"G3AnLNdRBjE", "libSceNpWebApi", "libSceNpWebApi", Addr(HOST_CALL(WebApi::sceNpWebApiInitialize))},
    {"KjNeZ-29ysQ", "libSceNpWebApi", "libSceNpWebApi", Addr(HOST_CALL(WebApi::sceNpWebApiSendRequest2))},
    {"VwJ5L0Higg0", "libSceNpWebApi", "libSceNpWebApi", Addr(HOST_CALL(WebApi::sceNpWebApiGetHttpResponseHeaderValue))},
    {"XUjdsSTTZ3U", "libSceNpWebApi", "libSceNpWebApi", Addr(HOST_CALL(WebApi::sceNpWebApiDeleteContext))},
    {"asz3TtIqGF8", "libSceNpWebApi", "libSceNpWebApi", Addr(HOST_CALL(WebApi::sceNpWebApiTerminate))},
    {"noQgleu+KLE", "libSceNpWebApi", "libSceNpWebApi", Addr(HOST_CALL(WebApi::sceNpWebApiDeleteRequest))},
    {"rdgs5Z1MyFw", "libSceNpWebApi", "libSceNpWebApi", Addr(HOST_CALL(WebApi::sceNpWebApiCreateRequest))},
    {"zk6c65xoyO0", "libSceNpWebApi", "libSceNpWebApi", Addr(HOST_CALL(WebApi::sceNpWebApiCreateContextA))},
    {"BtqmpTRXHgk", "libSceRtc", "libSceRtc", Addr(HOST_CALL(Rtc::sceRtcGetTime_t))},
    {"M1TvFst-jrM", "libSceRtc", "libSceRtc", Addr(HOST_CALL(Rtc::sceRtcConvertUtcToLocalTime))},
    {"ueega6v3GUw", "libSceRtc", "libSceRtc", Addr(HOST_CALL(Rtc::sceRtcSetTick))},
    {"zO9UL3qIINQ", "libSceRtc", "libSceRtc", Addr(HOST_CALL(Rtc::sceRtcGetCurrentNetworkTick))},
};
// clang-format on

} // namespace

std::span<const BridgeExport> AllBridgeExports() {
    static const std::vector<BridgeExport> all = [] {
        const auto generated = BridgeExports();
        std::vector<BridgeExport> v(generated.begin(), generated.end());
        v.insert(v.end(), std::begin(kFirstLightExports), std::end(kFirstLightExports));
        return v;
    }();
    return all;
}

} // namespace SecondSon::Bridge
