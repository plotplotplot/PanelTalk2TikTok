import bisect

import cv2
import numpy as np

import subtitle_render


def _line_length(words):
    total = 0
    for i, w in enumerate(words):
        wlen = len(w)
        total += wlen if i == 0 else wlen + 1
    return total


def _min_max_line_len(words):
    if len(words) <= 1:
        return _line_length(words)
    best = None
    for split in range(1, len(words)):
        left = _line_length(words[:split])
        right = _line_length(words[split:])
        m = max(left, right)
        if best is None or m < best:
            best = m
    return best if best is not None else 0


def build_word_groups(words, group_size=6, max_chars=None, subtitle_lines=1):
    groups = []
    current = []
    current_len = 0
    for idx, w in enumerate(words):
        text = (w.get("word") or "").strip()
        word_len = len(text)
        extra = word_len if current_len == 0 else word_len + 1
        if max_chars is not None and current:
            if subtitle_lines <= 1:
                if (current_len + extra) > max_chars:
                    groups.append(current)
                    current = []
                    current_len = 0
            else:
                candidate_words = [
                    (words[i].get("word") or "").strip() for i in (current + [idx])
                ]
                if _min_max_line_len(candidate_words) > max_chars:
                    groups.append(current)
                    current = []
                    current_len = 0
        current.append(idx)
        current_len += word_len if current_len == 0 else word_len + 1
        if len(current) >= group_size:
            groups.append(current)
            current = []
            current_len = 0
            continue
        if text.endswith((",", ".", "?", "!", ";", ":")):
            groups.append(current)
            current = []
            current_len = 0
    if current:
        groups.append(current)
    return groups


def build_subtitle_context(word_segment_times, group_size=6, max_chars=None, subtitle_lines=1):
    groups = build_word_groups(
        word_segment_times,
        group_size=group_size,
        max_chars=max_chars,
        subtitle_lines=subtitle_lines,
    )
    group_words_list = [
        [word_segment_times[i].get("word", "") for i in g] for g in groups
    ]
    group_for_idx = {}
    for gi, g in enumerate(groups):
        for pos, wi in enumerate(g):
            group_for_idx[wi] = (gi, pos)
    start_times = [float(w.get("start", 0.0)) for w in word_segment_times]
    end_times = [float(w.get("end", start_times[i])) for i, w in enumerate(word_segment_times)]
    return group_words_list, group_for_idx, start_times, end_times


def get_active_word_idx(t, start_times, end_times):
    if not start_times:
        return None
    idx = bisect.bisect_left(end_times, t)
    if idx >= len(end_times):
        return None
    if t < start_times[idx]:
        return None
    return idx


def apply_subtitles(
    frame,
    output_shape,
    group_words,
    active_idx,
    overlay_cache,
    mask_cache,
    bg_mask_cache,
    font_scale,
    text_height,
    text_height_2,
    subtitle_lines,
    font_ttf,
    subtitle_bg,
    subtitle_bg_color,
    subtitle_bg_alpha,
    subtitle_bg_pad,
    subtitle_bg_height,
    subtitle_bg_offset_y,
    shadow_size,
    shadow_offset_x,
    shadow_offset_y,
    subtitle_dilate,
    subtitle_dilate_size,
    subtitle_dilate_color,
    subtitle_text_color,
    subtitle_highlight_color,
    subtitle_shadow_color,
):
    cache_key = (
        tuple(group_words),
        active_idx,
        font_scale,
        text_height,
        text_height_2,
        subtitle_lines,
        bool(font_ttf),
        subtitle_bg,
        subtitle_bg_color,
        subtitle_bg_alpha,
        subtitle_bg_pad,
        subtitle_bg_height,
        subtitle_bg_offset_y,
        shadow_size,
        shadow_offset_x,
        shadow_offset_y,
        subtitle_dilate,
        subtitle_dilate_size,
        subtitle_dilate_color,
        subtitle_text_color,
        subtitle_highlight_color,
        subtitle_shadow_color,
    )
    if cache_key in overlay_cache:
        overlay = overlay_cache[cache_key]
        mask = mask_cache[cache_key]
        bg_mask = bg_mask_cache[cache_key]
    else:
        overlay, mask, bg_mask = subtitle_render.render_grouped_subtitle_overlay(
            output_shape,
            group_words,
            active_idx,
            font_scale=font_scale,
            text_height_from_bottom=text_height,
            text_height_from_bottom_2=text_height_2,
            subtitle_lines=subtitle_lines,
            font_ttf=font_ttf,
            subtitle_bg=subtitle_bg,
            subtitle_bg_color=subtitle_bg_color,
            subtitle_bg_alpha=subtitle_bg_alpha,
            subtitle_bg_pad=subtitle_bg_pad,
            subtitle_bg_full_width=True,
            subtitle_bg_height=subtitle_bg_height,
            subtitle_bg_offset_y=subtitle_bg_offset_y,
            shadow_size=shadow_size,
            shadow_offset_x=shadow_offset_x,
            shadow_offset_y=shadow_offset_y,
            dilate_size=subtitle_dilate_size if subtitle_dilate else 0,
            dilate_color=subtitle_dilate_color,
            text_color=subtitle_text_color,
            highlight_color=subtitle_highlight_color,
            shadow_color=subtitle_shadow_color,
        )
        overlay_cache[cache_key] = overlay
        mask_cache[cache_key] = mask
        bg_mask_cache[cache_key] = bg_mask

    if subtitle_bg and subtitle_bg_alpha > 0 and bg_mask is not None:
        alpha_bg = max(0.0, min(1.0, subtitle_bg_alpha))
        ys, xs = np.where(bg_mask > 0)
        if ys.size:
            y1, y2 = ys.min(), ys.max() + 1
            x1, x2 = xs.min(), xs.max() + 1
            roi = frame[y1:y2, x1:x2]
            roi_bg = overlay[y1:y2, x1:x2]
            frame[y1:y2, x1:x2] = cv2.addWeighted(
                roi, 1.0 - alpha_bg, roi_bg, alpha_bg, 0
            )

    if mask is not None:
        mask_indices = mask > 0
        frame[mask_indices] = overlay[mask_indices]
    return frame


def parse_bgr(value, name):
    parts = [int(x) for x in str(value).split(",")]
    if len(parts) != 3:
        raise ValueError(f"{name} must be in 'B,G,R' format")
    return (parts[0], parts[1], parts[2])
