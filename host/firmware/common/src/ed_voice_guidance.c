#include "ed_voice_guidance.h"
#include <stdio.h>
#include <string.h>

int ed_voice_build_response(const ed_snapshot_t *s, const ed_event_t *e, char *out, size_t cap, size_t *written)
{
    const char *status, *advice;
    int n; uint64_t age;
    if (!s || !out || !written || cap == 0u) return -1;
    switch (s->quality) {
    case ED_QUALITY_DISCONNECTED: status = "信号断线"; advice = "检查传感器连接和供电"; break;
    case ED_QUALITY_STALE: status = "信号过期"; advice = "等待新的采样快照"; break;
    case ED_QUALITY_SATURATED: status = "信号饱和"; advice = "检查量程和安装方式"; break;
    default:
        if (s->diagnosis == ED_DIAG_NO_BASELINE) { status = "尚未建立基线"; advice = "运行健康基线向导"; }
        else if (s->diagnosis == ED_DIAG_UNKNOWN_ANOMALY) { status = "发现未知异常"; advice = "查看频谱和原始留证，不强行归类"; }
        else if (s->diagnosis == ED_DIAG_UNCERTAIN) { status = "无法确定"; advice = "检查信号质量并重新采样"; }
        else { status = "基线范围内"; advice = "继续监测"; }
        break;
    }
    age = s->mono_us >= s->last_sample_mono_us ? s->mono_us - s->last_sample_mono_us : 0u;
    n = snprintf(out, cap, "{\"snapshot_id\":%llu,\"snapshot_age_us\":%llu,\"status\":\"%s\",\"advice\":\"%s\",\"event_id\":\"%s\",\"read_only\":true}",
                 (unsigned long long)s->snapshot_id, (unsigned long long)age,
                 status, advice, e ? e->event_id : "");
    if (n < 0 || (size_t)n >= cap) return -1;
    *written = (size_t)n;
    return 0;
}
