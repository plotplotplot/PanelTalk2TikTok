from faster_whisper import WhisperModel
import os, sys, json

infilename = sys.argv[1]
basename, _ = os.path.splitext(infilename)

model_size = "medium"

# Run on GPU with FP16
model = WhisperModel(model_size, device="cuda", compute_type="float16")

def process_file(infilename, process = False):
    segments, info = model.transcribe(
        infilename,
        beam_size=5,
        #word_timestamps=True,   # 👈 enable word boundaries
        language="en"           # 👈 force English
    )

    print("Detected language '%s' with probability %f" % (info.language, info.language_probability))

    results = {
        "language": info.language,
        "language_probability": info.language_probability,
        "segments": []
    }
    if not process:
        outfilename = infilename + ".txt"
        outstring = ""
        for segment in segments:
            outstring += "[%.2fs -> %.2fs] %s\n" % (segment.start, segment.end, segment.text)

        # Save txt
        with open(outfilename, "w", encoding="utf-8") as f:
            f.write(outstring)

        print(f"\nTXT transcript saved to {outfilename}")

    else:
        outfilename = infilename + ".json"
        for segment in segments:
            print("[%.2fs -> %.2fs] %s" % (segment.start, segment.end, segment.text))
            seg_data = {
                "start": segment.start,
                "end": segment.end,
                "text": segment.text,
                "words": []
            }
            for word in segment.words:
                #print("   [%.2fs -> %.2fs] %s" % (word.start, word.end, word.word))
                seg_data["words"].append({
                    "start": word.start,
                    "end": word.end,
                    "word": word.word
                })
            results["segments"].append(seg_data)

        # Save JSON
        with open(outfilename, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=2, ensure_ascii=False)

        print(f"\nJSON transcript saved to {outfilename}")

if os.path.isfile(infilename):
    process_file(infilename)

else:
    for videofile in os.listdir(infilename):
        if videofile.lower().endswith(".mov") or videofile.lower().endswith(".mp4") or videofile.lower().endswith(".avi"):
            fullfile = os.path.join(infilename, videofile)
            process_file(fullfile)
