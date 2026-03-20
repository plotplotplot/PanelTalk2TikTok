def format_len_diff(label, idx, expected, actual, unit="frames"):
    diff = actual - expected
    ok = diff == 0
    status = "OK" if ok else "DIFF"
    return f"[len] {label} seg={idx} expected={expected} actual={actual} diff={diff} {unit} {status}"


def log_len_diff(label, idx, expected, actual, unit="frames"):
    print(format_len_diff(label, idx, expected, actual, unit=unit))
