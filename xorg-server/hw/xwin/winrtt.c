/*
 * Copyright (C) 2025 - VcXsrv RTT Optimization
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_XWIN_CONFIG_H
#include <xwin-config.h>
#endif

#include "win.h"
#include <stdio.h>
#include <string.h>

/* Global RTT configuration */
WinRTTConfig g_winRTTConfig;

/*
 * Adaptive parameter adjustment for extreme variance networks
 */
static void
winAdaptiveAdjustParameters(void)
{
    DWORD current_time = GetTickCount();
    
    /* Don't adjust too frequently - minimum 2 seconds between adjustments */
    if (current_time - g_winRTTConfig.last_adjustment < 2000) {
        return;
    }
    
    g_winRTTConfig.last_adjustment = current_time;
    
    /* Enhanced adaptive logic for ultra-low to high RTT */
    int current_rtt = g_winRTTConfig.recent_rtt_samples[g_winRTTConfig.sample_index];
    if (current_rtt > 50) {
        /* High RTT: increase buffer */
        g_winRTTConfig.adaptive_buffer_size = 1024 * 1024; /* 1MB */
        g_winRTTConfig.flush_threshold_pct = 40; /* Very aggressive batching */
        g_winRTTConfig.batch_count_threshold = 20;
        g_winRTTConfig.time_threshold_ms = 100;
        g_winRTTConfig.poll_timeout_ms = 60;
        g_winRTTConfig.flush_hysteresis = 8;
    } else if (current_rtt > 25) {
        /* Medium RTT: moderate settings */
        g_winRTTConfig.adaptive_buffer_size = 512 * 1024; /* 512KB */
        g_winRTTConfig.flush_threshold_pct = 60;
        g_winRTTConfig.batch_count_threshold = 12;
        g_winRTTConfig.time_threshold_ms = 70;
        g_winRTTConfig.poll_timeout_ms = 40;
        g_winRTTConfig.flush_hysteresis = 5;
    } else if (current_rtt > 15) {
        /* Low RTT: responsive settings */
        g_winRTTConfig.adaptive_buffer_size = 128 * 1024; /* 128KB */
        g_winRTTConfig.flush_threshold_pct = 85;
        g_winRTTConfig.batch_count_threshold = 4;
        g_winRTTConfig.time_threshold_ms = 25;
        g_winRTTConfig.poll_timeout_ms = 15;
        g_winRTTConfig.flush_hysteresis = 2;
    } else {
        /* Ultra-low RTT (8-15ms): Maximum responsiveness with micro-optimizations */
        g_winRTTConfig.adaptive_buffer_size = 64 * 1024; /* 64KB - smaller for speed */
        g_winRTTConfig.flush_threshold_pct = 95; /* Flush almost immediately */
        g_winRTTConfig.batch_count_threshold = 2; /* Micro-batching only */
        g_winRTTConfig.time_threshold_ms = 8; /* Ultra-fast timeout */
        g_winRTTConfig.poll_timeout_ms = 5; /* Very responsive polling */
        g_winRTTConfig.flush_hysteresis = 1; /* Immediate flush */
    }
    
    /* Enable GDI batching for high variance networks */
    g_winRTTConfig.enable_gdi_batching = (g_winRTTConfig.rtt_variance > 500);
    
    /* Enable Nagle for very high RTT */
    g_winRTTConfig.enable_nagle = (current_rtt > 40);
    
    winDebug("ADAPTIVE: Adjusted parameters for RTT=%dms, variance=%d\n", 
             current_rtt, g_winRTTConfig.rtt_variance);
}

/*
 * Auto-detect RTT and switch modes accordingly
 */
void
winAutoDetectRTT(void)
{
    static int poll_count = 0;
    static DWORD last_poll_time = 0;
    static int mode_switch_cooldown = 0;
    
    DWORD current_time = GetTickCount();
    
    /* Simple RTT estimation based on poll timing variance */
    if (last_poll_time > 0) {
        int poll_delta = current_time - last_poll_time;
        
        /* Update rolling window of RTT samples */
        g_winRTTConfig.recent_rtt_samples[g_winRTTConfig.sample_index] = poll_delta;
        g_winRTTConfig.sample_index = (g_winRTTConfig.sample_index + 1) % 20;
        
        /* Calculate variance every 20 samples */
        if ((++poll_count % 20) == 0) {
            int sum = 0, mean, variance = 0;
            int i;
            
            /* Calculate mean */
            for (i = 0; i < 20; i++) {
                sum += g_winRTTConfig.recent_rtt_samples[i];
            }
            mean = sum / 20;
            
            /* Calculate variance */
            for (i = 0; i < 20; i++) {
                int diff = g_winRTTConfig.recent_rtt_samples[i] - mean;
                variance += diff * diff;
            }
            g_winRTTConfig.rtt_variance = variance / 20;
            
            winDebug("RTT Auto-detect: mean=%dms, variance=%d\n", mean, g_winRTTConfig.rtt_variance);
            
            /* Check if we should switch to adaptive mode for high variance */
            if (g_winRTTConfig.rtt_variance > 1000 && g_winRTTConfig.mode != RTT_MODE_ADAPTIVE) {
                winDebug("AUTO: High variance detected (%d), switching to ADAPTIVE mode\n", g_winRTTConfig.rtt_variance);
                winInitializeRTTConfig(RTT_MODE_ADAPTIVE);
                g_winRTTConfig.mode = RTT_MODE_AUTO; /* Keep auto flag for monitoring */
                mode_switch_cooldown = 100; /* Prevent rapid switching */
            }
        }
        
        /* Auto mode: switch to appropriate fixed mode if variance is low */
        if (g_winRTTConfig.mode == RTT_MODE_AUTO) {
            if (g_winRTTConfig.rtt_variance < 100) { /* Low variance - use fixed mode */
                WinRTTMode new_mode;
                int estimated_rtt = g_winRTTConfig.recent_rtt_samples[g_winRTTConfig.sample_index];
                if (estimated_rtt < 15) {
                    new_mode = RTT_MODE_ULTRA;
                } else if (estimated_rtt < 25) {
                    new_mode = RTT_MODE_LOW;
                } else if (estimated_rtt < 35) {
                    new_mode = RTT_MODE_STANDARD;
                } else {
                    new_mode = RTT_MODE_HIGH;
                }
                
                winDebug("AUTO: Switching to fixed mode %d (RTT=%dms, variance=%d)\n",
                         new_mode, estimated_rtt, g_winRTTConfig.rtt_variance);
                winInitializeRTTConfig(new_mode);
                g_winRTTConfig.mode = RTT_MODE_AUTO; /* Keep auto flag */
            }
        }
        
        /* For adaptive mode, run frequent parameter adjustment */
        if (g_winRTTConfig.mode == RTT_MODE_ADAPTIVE) {
            winAdaptiveAdjustParameters();
        }
    }
    
    last_poll_time = current_time;
    
    if (mode_switch_cooldown > 0) {
        mode_switch_cooldown--;
    }
}

/*
 * Initialize RTT configuration for specific mode
 */
void 
winInitializeRTTConfig(WinRTTMode mode)
{
    g_winRTTConfig.mode = mode;
    g_winRTTConfig.rtt_variance = 0;
    g_winRTTConfig.sample_index = 0;
    g_winRTTConfig.last_adjustment = GetTickCount();
    memset(g_winRTTConfig.recent_rtt_samples, 0, sizeof(g_winRTTConfig.recent_rtt_samples));
    
    switch (mode) {
    case RTT_MODE_ULTRA:
        /* RTT < 15ms - ultra-low latency optimizations for premium LAN */
        g_winRTTConfig.socket_buffer_size = 32 * 1024;      /* 32KB - ultra-small for speed */
        g_winRTTConfig.flush_threshold_pct = 98;            /* 98% - almost immediate */
        g_winRTTConfig.batch_count_threshold = 1;           /* Single write - no batching */
        g_winRTTConfig.time_threshold_ms = 5;               /* 5ms - ultra-fast */
        g_winRTTConfig.poll_timeout_ms = 3;                 /* 3ms - ultra-responsive */
        g_winRTTConfig.flush_hysteresis = 1;                /* 1 pending - immediate */
        g_winRTTConfig.enable_nagle = FALSE;                /* TCP_NODELAY critical */
        g_winRTTConfig.enable_gdi_batching = FALSE;         /* No batching overhead */
        winDebug("RTT Config: ULTRA mode (RTT < 15ms - Maximum responsiveness)\n");
        break;
        
    case RTT_MODE_LOW:
        /* RTT 15-25ms - minimal optimizations for good LAN */
        g_winRTTConfig.socket_buffer_size = 64 * 1024;      /* 64KB */
        g_winRTTConfig.flush_threshold_pct = 90;            /* 90% - flush less */
        g_winRTTConfig.batch_count_threshold = 3;           /* 3 writes */
        g_winRTTConfig.time_threshold_ms = 20;              /* 20ms */
        g_winRTTConfig.poll_timeout_ms = 8;                 /* 8ms - responsive */
        g_winRTTConfig.flush_hysteresis = 2;                /* 2 pending */
        g_winRTTConfig.enable_nagle = FALSE;                /* Keep TCP_NODELAY */
        g_winRTTConfig.enable_gdi_batching = FALSE;         /* No GDI batching */
        winDebug("RTT Config: LOW mode (RTT 15-25ms)\n");
        break;
        
    case RTT_MODE_STANDARD:
        /* RTT 25-35ms - standard optimizations */
        g_winRTTConfig.socket_buffer_size = 256 * 1024;     /* 256KB */
        g_winRTTConfig.flush_threshold_pct = 75;            /* 75% */
        g_winRTTConfig.batch_count_threshold = 8;           /* 8 writes */
        g_winRTTConfig.time_threshold_ms = 50;              /* 50ms */
        g_winRTTConfig.poll_timeout_ms = 25;                /* 25ms */
        g_winRTTConfig.flush_hysteresis = 3;                /* 3 pending */
        g_winRTTConfig.enable_nagle = FALSE;                /* TCP_NODELAY */
        g_winRTTConfig.enable_gdi_batching = FALSE;         /* Per-box blitting */
        winDebug("RTT Config: STANDARD mode (RTT 25-35ms)\n");
        break;
        
    case RTT_MODE_HIGH:
        /* RTT 35-50ms - aggressive optimizations */
        g_winRTTConfig.socket_buffer_size = 512 * 1024;     /* 512KB */
        g_winRTTConfig.flush_threshold_pct = 50;            /* 50% - early batching */
        g_winRTTConfig.batch_count_threshold = 15;          /* 15 writes */
        g_winRTTConfig.time_threshold_ms = 80;              /* 80ms */
        g_winRTTConfig.poll_timeout_ms = 50;                /* 50ms */
        g_winRTTConfig.flush_hysteresis = 6;                /* 6 pending */
        g_winRTTConfig.enable_nagle = TRUE;                 /* Enable Nagle */
        g_winRTTConfig.enable_gdi_batching = TRUE;          /* Batch GDI operations */
        winDebug("RTT Config: HIGH mode (RTT 35-50ms)\n");
        break;
        
    case RTT_MODE_ADAPTIVE:
        /* RTT 10-100ms+ - extreme variance adaptive */
        g_winRTTConfig.socket_buffer_size = 256 * 1024;     /* Start with standard */
        g_winRTTConfig.flush_threshold_pct = 70;            /* Moderate */
        g_winRTTConfig.batch_count_threshold = 10;          /* Moderate */
        g_winRTTConfig.time_threshold_ms = 50;              /* Moderate */
        g_winRTTConfig.poll_timeout_ms = 30;                /* Moderate */
        g_winRTTConfig.flush_hysteresis = 4;                /* Moderate */
        g_winRTTConfig.enable_nagle = TRUE;                 /* Usually helps */
        g_winRTTConfig.enable_gdi_batching = TRUE;          /* Usually helps */
        g_winRTTConfig.adaptive_buffer_size = g_winRTTConfig.socket_buffer_size;
        winDebug("RTT Config: ADAPTIVE mode (RTT 10-100ms+ extreme variance)\n");
        break;
        
    case RTT_MODE_AUTO:
    default:
        /* Default to standard, auto-detection will adjust */
        winInitializeRTTConfig(RTT_MODE_STANDARD);
        g_winRTTConfig.mode = RTT_MODE_AUTO; /* Override to AUTO after setting defaults */
        winDebug("RTT Config: AUTO mode (auto-detection enabled)\n");
        break;
    }
}

/*
 * Set RTT mode from string argument
 */
void
winSetRTTMode(const char *mode_str)
{
    if (!mode_str) {
        winInitializeRTTConfig(RTT_MODE_AUTO);
        return;
    }
    
    if (strcmp(mode_str, "ultra") == 0) {
        winInitializeRTTConfig(RTT_MODE_ULTRA);
    } else if (strcmp(mode_str, "low") == 0) {
        winInitializeRTTConfig(RTT_MODE_LOW);
    } else if (strcmp(mode_str, "standard") == 0) {
        winInitializeRTTConfig(RTT_MODE_STANDARD);
    } else if (strcmp(mode_str, "high") == 0) {
        winInitializeRTTConfig(RTT_MODE_HIGH);
    } else if (strcmp(mode_str, "adaptive") == 0) {
        winInitializeRTTConfig(RTT_MODE_ADAPTIVE);
    } else if (strcmp(mode_str, "auto") == 0) {
        winInitializeRTTConfig(RTT_MODE_AUTO);
    } else {
        winDebug("Invalid RTT mode '%s', using auto\n", mode_str);
        winInitializeRTTConfig(RTT_MODE_AUTO);
    }
}

/*
 * Get current effective socket buffer size (for adaptive mode)
 */
int
winGetEffectiveSocketBuffer(void)
{
    if (g_winRTTConfig.mode == RTT_MODE_ADAPTIVE) {
        return g_winRTTConfig.adaptive_buffer_size;
    }
    return g_winRTTConfig.socket_buffer_size;
}

/*
 * Get RTT enable nagle setting (for Xtrans layer)
 */
int
winGetRTTEnableNagle(void)
{
    return g_winRTTConfig.enable_nagle ? 1 : 0;
}

/*
 * Check if RTT mode is HIGH (for clipboard polling optimization)
 */
int
winIsRTTModeHigh(void)
{
    return (g_winRTTConfig.mode == RTT_MODE_HIGH || g_winRTTConfig.mode == RTT_MODE_ADAPTIVE);
}
