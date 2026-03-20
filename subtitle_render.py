import numpy as np
import cv2
from PIL import Image, ImageDraw, ImageFont


def render_grouped_subtitle_overlay(
    frame_shape,
    words,
    active_idx,
    font_scale=1.8,
    text_height_from_bottom=300,
    text_height_from_bottom_2=None,
    subtitle_lines=1,
    font_ttf=None,
    subtitle_bg=False,
    subtitle_bg_color=(0, 0, 0),
    subtitle_bg_alpha=0.6,
    subtitle_bg_pad=8,
    subtitle_bg_full_width=True,
    subtitle_bg_height=0,
    subtitle_bg_offset_y=0,
    shadow_size=0,
    shadow_offset_x=0,
    shadow_offset_y=0,
    dilate_size=0,
    dilate_color=(0, 0, 0),
    text_color=(255, 255, 255),
    highlight_color=(0, 255, 255),
    shadow_color=(0, 0, 0),
    line_spacing_mult=1.2,
):
    """
    Render grouped subtitle text once and return (overlay, mask).
    """
    height, width = frame_shape[:2]
    thickness = int(font_scale * 2)
    font = cv2.FONT_HERSHEY_SIMPLEX
    use_ttf = font_ttf is not None

    def measure_text(word):
        if use_ttf:
            bbox = font_ttf.getbbox(word)
            return (bbox[2] - bbox[0], bbox[3] - bbox[1])
        return cv2.getTextSize(word, font, font_scale, thickness)[0]

    safe_words = []
    for w in words:
        if w is None:
            safe_words.append("")
        elif isinstance(w, float) and (np.isnan(w) or np.isinf(w)):
            safe_words.append("")
        else:
            safe_words.append(str(w))

    word_sizes = [measure_text(w) for w in safe_words]
    space_width = measure_text(" ")[0]

    overlay = np.zeros((height, width, 3), dtype=np.uint8)
    mask = np.zeros((height, width), dtype=np.uint8)
    bg_mask = np.zeros((height, width), dtype=np.uint8)

    if subtitle_lines <= 1 or len(safe_words) <= 1:
        lines = [(safe_words, active_idx, height - text_height_from_bottom)]
    else:
        if text_height_from_bottom_2 is None:
            line_height = measure_text("Hg")[1]
            text_height_from_bottom_2 = text_height_from_bottom + int(
                round(line_height * float(line_spacing_mult))
            )
        # Split words into two balanced lines by width
        if len(safe_words) == 2:
            split_idx = 1
        else:
            cumulative = []
            total = 0
            for i, sz in enumerate(word_sizes):
                total += sz[0] + (space_width if i > 0 else 0)
                cumulative.append(total)
            best_idx = 1
            best_diff = None
            for i in range(1, len(safe_words)):
                left = cumulative[i - 1]
                right = total - cumulative[i - 1] - space_width
                diff = abs(left - right)
                if best_diff is None or diff < best_diff:
                    best_diff = diff
                    best_idx = i
            split_idx = best_idx

        line1_words = safe_words[:split_idx]
        line2_words = safe_words[split_idx:]
        if active_idx < split_idx:
            line1_active = active_idx
            line2_active = -1
        else:
            line1_active = -1
            line2_active = active_idx - split_idx

        lines = [
            (line1_words, line1_active, height - text_height_from_bottom_2),
            (line2_words, line2_active, height - text_height_from_bottom),
        ]

    shadow_offset_x = int(shadow_offset_x)
    shadow_offset_y = int(shadow_offset_y)

    if use_ttf:
        overlay_img = Image.fromarray(overlay)
        mask_img = Image.fromarray(mask)
        draw = ImageDraw.Draw(overlay_img)
        draw_mask = ImageDraw.Draw(mask_img)

    def draw_bg_rect(x1, x2, y1, y2):
        if not subtitle_bg or subtitle_bg_alpha <= 0:
            return
        if subtitle_bg_full_width:
            x1 = 0
            x2 = width
        x1 = max(0, int(x1))
        x2 = min(width, int(x2))
        y1 = max(0, int(y1))
        y2 = min(height, int(y2))
        if x2 <= x1 or y2 <= y1:
            return
        b, g, r = subtitle_bg_color
        alpha = max(0.0, min(1.0, subtitle_bg_alpha))
        if use_ttf:
            rect = Image.new("RGB", (x2 - x1, y2 - y1), (r, g, b))
            overlay_img.paste(rect, (x1, y1))
            bg_mask[y1:y2, x1:x2] = 255
        else:
            overlay[y1:y2, x1:x2] = (b, g, r)
            bg_mask[y1:y2, x1:x2] = 255

    def draw_word(x, y, word, color):
        if use_ttf:
            # Shadow
            if shadow_size > 0 and dilate_size <= 0:
                draw.text(
                    (x + shadow_offset_x, y + shadow_offset_y),
                    word,
                    font=font_ttf,
                    fill=shadow_color,
                )
                draw_mask.text((x + shadow_offset_x, y + shadow_offset_y), word, font=font_ttf, fill=255)
            # Text
            draw.text((x, y), word, font=font_ttf, fill=color)
            draw_mask.text((x, y), word, font=font_ttf, fill=255)
        else:
            if shadow_size > 0 and dilate_size <= 0:
                cv2.putText(
                    overlay,
                    word,
                    (x + shadow_offset_x, y + shadow_offset_y),
                    font,
                    font_scale,
                    shadow_color,
                    thickness,
                    cv2.LINE_AA,
                )
                cv2.putText(
                    mask,
                    word,
                    (x + shadow_offset_x, y + shadow_offset_y),
                    font,
                    font_scale,
                    255,
                    thickness,
                    cv2.LINE_AA,
                )
            cv2.putText(
                overlay,
                word,
                (x, y),
                font,
                font_scale,
                color,
                thickness,
                cv2.LINE_AA,
            )
            cv2.putText(
                mask,
                word,
                (x, y),
                font,
                font_scale,
                255,
                thickness,
                cv2.LINE_AA,
            )

    # Draw background first (behind text) as a single bar
    if subtitle_bg:
        line_metas = []
        for line_words, _, y in lines:
            if not line_words:
                continue
            line_sizes = [measure_text(w) for w in line_words]
            total_width = sum(w[0] for w in line_sizes) + space_width * max(0, len(line_words) - 1)
            line_height = max((h for _, h in line_sizes), default=0)
            x = (width - total_width) // 2
            line_metas.append((x, y, total_width, line_height))

        if line_metas:
            min_top = min(y - h for _, y, _, h in line_metas)
            base_y = max(y for _, y, _, _ in line_metas)
            auto_height = (base_y - min_top) + 2 * subtitle_bg_pad
            if subtitle_bg_height and subtitle_bg_height > 0:
                # Treat height as an absolute distance from the bottom of the frame.
                bar_height = subtitle_bg_height
                y2 = height + subtitle_bg_offset_y
                y1 = y2 - bar_height
            else:
                bar_height = auto_height
                y2 = base_y + subtitle_bg_offset_y
                y1 = y2 - bar_height
            # Use full-width bar by default; offsets are relative to bottom edge (y2)
            draw_bg_rect(0, width, y1, y2)

    # Draw text over background
    for line_words, line_active, y in lines:
        if not line_words:
            continue
        line_sizes = [measure_text(w) for w in line_words]
        total_width = sum(w[0] for w in line_sizes) + space_width * max(0, len(line_words) - 1)
        x = (width - total_width) // 2
        for i, word in enumerate(line_words):
            if not word and i != len(line_words) - 1:
                x += space_width
                continue
            color = highlight_color if i == line_active else text_color
            draw_word(x, y, word, color)
            x += line_sizes[i][0] + space_width

    if use_ttf:
        overlay = np.array(overlay_img)
        mask = np.array(mask_img)

    if dilate_size and dilate_size > 0:
        k = int(dilate_size) * 2 + 1
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (k, k))
        dilated = cv2.dilate(mask, kernel)
        outline_only = (dilated > 0) & (mask == 0)
        if np.any(outline_only):
            overlay[outline_only] = dilate_color
            mask[outline_only] = 255

    return overlay, mask, bg_mask


def compact_overlay_region(overlay, mask, bg_mask=None):
    """
    Crop rendered subtitle buffers to the smallest region that contains
    visible subtitle pixels or subtitle background pixels.
    """
    if overlay is None or mask is None:
        return None, None, None, None

    nonzero = mask > 0
    if bg_mask is not None:
        nonzero |= bg_mask > 0

    ys, xs = np.where(nonzero)
    if ys.size == 0 or xs.size == 0:
        return None, None, None, None

    y1, y2 = ys.min(), ys.max() + 1
    x1, x2 = xs.min(), xs.max() + 1

    overlay_roi = overlay[y1:y2, x1:x2].copy()
    mask_roi = mask[y1:y2, x1:x2].copy()
    bg_mask_roi = None
    if bg_mask is not None:
        bg_mask_roi = bg_mask[y1:y2, x1:x2].copy()

    return overlay_roi, mask_roi, bg_mask_roi, (x1, y1)
