/*
 * stand_main.cpp — UTAU resampler protocol front-end for vConnect-STAND.
 *
 * vConnect-STAND originally spoke only its own CLI: `-i <vsq_meta.txt> -o <out.wav>`
 * (synthesis), `-c` (convert a UTAU oto.ini into a vConnect library) and `-t`
 * (transcribe). Neither shape is what a UTAU resampler is invoked with, so
 * OpenUtau's ExeResampler could not drive it.
 *
 * This front-end accepts the standard resampler argument vector
 *
 *     <in.wav> <out.wav> <note> <velocity> <flags> <offset> <length>
 *     <consonant> <cutoff> <volume> <modulation> !<tempo> <pitchbend>
 *
 * builds the equivalent VSQ meta-text in a temporary file, and hands it to the
 * unchanged vConnect Synthesizer. When the first argument is not a resampler
 * call, the original vConnect CLI is preserved verbatim, so this binary remains
 * a drop-in replacement for the upstream vConnect-STAND.
 *
 * Note on the pitch field: OpenUtau passes the note as a tone name (e.g. "C4"),
 * while vConnect's Sequence speaks MIDI note numbers. Both forms are accepted.
 *
 * This file is part of vConnect-STAND and is GPL, like the rest of it.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <unistd.h>

#include "RuntimeOption.h"
#include "Synthesizer.h"
#include "utau/UtauParameter.h"

namespace {

// ---- small helpers -------------------------------------------------------

bool fileExists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

// "C4" / "C#4" / "Bb4" -> MIDI note number. Returns -1 when unparsable.
int toneNameToNote(const std::string& s) {
    if (s.empty()) return -1;
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    char letter = (char)toupper((unsigned char)s[0]);
    int idx = -1;
    const char* letters = "ABCDEFG";
    for (int i = 0; i < 7; i++) {
        if (letters[i] == letter) { idx = i; break; }
    }
    if (idx < 0) return -1;
    size_t pos = 1;
    int accidental = 0;
    while (pos < s.size() && (s[pos] == '#' || s[pos] == 'b' ||
                              s[pos] == '+' || s[pos] == '-')) {
        accidental += (s[pos] == '#' || s[pos] == '+') ? 1 : -1;
        pos++;
    }
    if (pos >= s.size()) return -1;
    // Octave digits may be negative (C-1).
    int octaveOffset = 0;
    if (s[pos] == '-' || s[pos] == '+') {
        octaveOffset = (s[pos] == '-') ? -1 : 1;
        pos++;
    }
    if (pos >= s.size() || !isdigit((unsigned char)s[pos])) return -1;
    int octave = atoi(s.c_str() + pos) * (octaveOffset < 0 ? -1 : 1);
    // C4 == 60 => (octave + 1) * 12 + semitone.
    return (octave + 1) * 12 + base[idx] + accidental;
}

// Accepts either a tone name or a plain integer.
int parseNote(const std::string& s) {
    if (s.empty()) return 60;
    bool numeric = true;
    size_t start = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (start >= s.size()) numeric = false;
    for (size_t i = start; i < s.size(); i++) {
        if (!isdigit((unsigned char)s[i])) { numeric = false; break; }
    }
    if (numeric) {
        int n = atoi(s.c_str());
        if (n >= 0 && n < 128) return n;
    }
    int note = toneNameToNote(s);
    return (note >= 0 && note < 128) ? note : 60;
}

std::string dirName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? std::string(".") : path.substr(0, p);
}

std::string baseName(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

// ---- oto.ini lookup ------------------------------------------------------
//
// vConnect resolves a note's corpus file by looking the lyric up in the
// bank's oto.ini (utau/Oto.cpp doFind matches both the alias and the file
// name). The front-end needs the same lookup to recover the preutterance
// timing — it is NOT part of the resampler argument list — and the alias to
// put in the VSQ. Fields are parsed in UtauParameter's order:
//     file=alias,offset,fixed,cutoff,preutter,overlap

struct OtoEntry {
    std::string alias;
    double preutterMs;
    double overlapMs;
    bool found;
    OtoEntry() : preutterMs(0.0), overlapMs(0.0), found(false) {}
};

bool findOtoEntry(const std::string& otoPath, const std::string& fileName,
                  OtoEntry& out) {
    std::ifstream in(otoPath.c_str(), std::ios::binary);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || line.substr(0, eq) != fileName) continue;
        std::string rest = line.substr(eq + 1);
        size_t comma = rest.find(',');
        if (comma == std::string::npos) continue;
        double v[5] = { 0, 0, 0, 0, 0 };
        if (sscanf(rest.c_str() + comma + 1, "%lf,%lf,%lf,%lf,%lf",
                   &v[0], &v[1], &v[2], &v[3], &v[4]) != 5) continue;
        out.alias = rest.substr(0, comma);
        out.preutterMs = v[3];
        out.overlapMs = v[4];
        out.found = true;
        return true;
    }
    return false;
}

// The alias travels into the VSQ as raw oto.ini bytes, and vConnect's Oto
// converts the same bytes with encodingOtoIni before comparing. Decoding the
// VSQ with that SAME encoding makes the two transformations identical, so the
// lookup matches for any bank encoding — which is why detection here only has
// to be consistent, not perfect. UTF-8 BOM or valid UTF-8 wins; otherwise the
// UTAU default Shift_JIS.
std::string detectOtoEncoding(const std::string& otoPath) {
    std::ifstream in(otoPath.c_str(), std::ios::binary);
    if (!in) return "Shift_JIS";
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        return "UTF-8";
    }
    size_t i = 0, n = data.size();
    while (i < n) {
        unsigned char c = (unsigned char)data[i];
        if (c < 0x80) { i++; continue; }
        int extra;
        unsigned int cp;
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else { return "Shift_JIS"; }
        if (i + (size_t)extra >= n) return "Shift_JIS";
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)data[i + k];
            if ((cc & 0xC0) != 0x80) return "Shift_JIS";
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return "Shift_JIS";
        }
        i += (size_t)extra + 1;
    }
    return "UTF-8";
}

// ---- VSQ meta-text emission ---------------------------------------------
//
// The format is line-oriented, `key=value`, with `[ObjectName]` section
// headers (see vsq/Sequence.cpp readCore()). Only the subset the resampler
// path needs is emitted.

std::string escapeLyric(const std::string& lyric) {
    // The lyric travels inside quotes on its own line; only a newline could
    // break the line structure. findOtoEntry() can never yield one (it splits
    // on newlines), so this is defensive. Quotes, commas and brackets are
    // legitimate alias content and Lyric::Lyric() slices first-to-last quote,
    // so they pass through untouched.
    std::string out;
    for (size_t i = 0; i < lyric.size(); i++) {
        char c = lyric[i];
        if (c == '\n' || c == '\r') continue;
        out += c;
    }
    return out;
}

struct NoteRequest {
    int note;        // MIDI number
    int velocity;    // 0..127
    double offsetMs;
    double lengthMs;
    double consonantMs;
    double preutterMs;
    double overlapMs;
    double tempo;
    std::string flags;
};

std::string buildVsq(const NoteRequest& req, const std::string& wavPath,
                     const std::string& lyric) {
    // vConnect reads tempo from [TempoList] and note timing in ticks at 480
    // per quarter note.
    const int RESOLUTION = 480;
    const double msPerTick = 60000.0 / (req.tempo * RESOLUTION);

    long lengthTicks = (long)llround(req.lengthMs / msPerTick);
    if (lengthTicks < 1) lengthTicks = 1;

    // vConnect's velocity scale: VSQ Dynamics 64 = normal, stretch =
    // 2^((64-D)/64) (Synthesizer::calculateVsqInfo). OpenUtau passes
    // velocity 0..200 with 100 = normal and stretch 2^((100-v)/100), so the
    // two match at D = 0.64 * v (100 -> 64, both stretch 1.0).
    int dynamics = (int)llround(0.64 * (double)req.velocity);
    if (dynamics < 1) dynamics = 1;
    if (dynamics > 127) dynamics = 127;
    double stretch = pow(2.0, (64.0 - dynamics) / 64.0);

    // Place the note one effective preutterance INTO the sequence so that
    // vConnect's beginFrame lands on 0: the synthesis window then starts at
    // exactly (note - preutterance), which is where OpenUtau's SharpWavtool
    // expects resampler sample 0 (segment.posMs = positionMs - leadingMs).
    // A note at tick 0 with PreUtterance > 0 gives beginFrame < 0, which is
    // the historical segfault's precondition (the head-drop safety net in
    // WaveBuffer::writeWaveFile covers the residual ±1 frame of rounding).
    double preutterEffMs = req.preutterMs * stretch;
    long startTicks = (long)llround(preutterEffMs / msPerTick);
    if (startTicks < 0) startTicks = 0;

    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(6);

    // [EventList] must come first: it is what registers the [#ID] and [h#ID]
    // sections in the parser's maps, and any section referenced before it is
    // registered is dropped with "not found". An `EOS` entry closes the list.
    // Event IDs are `ID#nnnn`, which EventList::setParameter wraps into the
    // section key `[ID#nnnn]`; Sequence::readCore matches those with
    // find("[ID#"). A bare `[#nnnn]` never matches and is dropped.
    o << "[EventList]\n";
    o << startTicks << "=ID#0001\n";
    o << "" << (startTicks + lengthTicks) << "=EOS\n";

    o << "[ID#0001]\n";
    o << "Type=Anote\n";
    o << "Length=" << lengthTicks << "\n";
    o << "Note#=" << req.note << "\n";
    o << "Dynamics=" << dynamics << "\n";
    o << "PMBendDepth=8\n";
    o << "PMBendLength=0\n";
    o << "PMbPortamentoUse=0\n";
    o << "DEMdecGainRate=50\n";
    o << "DEMaccent=50\n";
    o << "PreUtterance=" << req.preutterMs << "\n";
    o << "VoiceOverlap=" << req.overlapMs << "\n";
    o << "LyricHandle=h#0001\n";

    o << "[h#0001]\n";
    // A LyricHandle carries exactly one lyric, L0. Handle::setParameter routes
    // *any* key containing 'L' to the lyric, so emitting L1..L4 (as VOCALOID
    // VSQ does for vibrato handles) would silently overwrite it — L4=0 sets the
    // lyric to the string "0". Only L0 belongs here, in the quoted form that
    // Lyric::Lyric() expects.
    o << "L0=\"" << escapeLyric(lyric) << "\"\n";

    o << "[TempoList]\n";
    o << "0.00=" << req.tempo << "\n";

    // [oto.ini] takes the path of oto.ini itself (Sequence::setParamOtoIni
    // hands it straight to UtauDB, which parses that file), not the directory
    // containing it. Relative paths resolve against the meta-text's directory.
    o << "[oto.ini]\n";
    o << dirName(wavPath) << "/oto.ini\n";

    return o.str();
}

int runSynthesizer(const std::string& vsqPath, const std::string& outWav,
                   const std::string& encoding) {
    // RuntimeOption has no setters: it is built from argv and nothing else.
    // Both readers get the oto.ini encoding: Oto converts the oto.ini with it,
    // and the VSQ reader converts the raw alias bytes with it too, so both
    // sides end up with the identical string and doFind() matches.
    std::string inCopy = vsqPath;
    std::string outCopy = outWav;
    std::string encCopy = encoding;
    char arg0[] = "vConnect-STAND";
    char arg1[] = "-i";
    char arg3[] = "-o";
    char arg5[] = "-charset-vxt";
    char arg7[] = "-charset-otoini";
    char* argv[] = { arg0, arg1, const_cast<char*>(inCopy.c_str()), arg3,
                     const_cast<char*>(outCopy.c_str()), arg5,
                     const_cast<char*>(encCopy.c_str()), arg7,
                     const_cast<char*>(encCopy.c_str()), NULL };
    vconnect::RuntimeOption option(9, argv);
    vconnect::Synthesizer synth(option);
    synth.run();
    return fileExists(outWav) ? 0 : 1;
}

void printResamplerUsage(const char* argv0) {
    std::cerr
        << "vConnect-STAND resampler front-end\n"
        << "  " << argv0 << " <in.wav> <out.wav> <note> <velocity> <flags> "
        << "<offset> <length> <consonant> <cutoff> <volume> <modulation> "
        << "!<tempo> <pitchbend>\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    // A resampler invocation has at least the 13 documented positional args.
    // Anything else falls through to the original vConnect CLI so existing
    // `-i/-o`, `-c` and `-t` usage keeps working.
    bool looksLikeResampler =
        argc >= 13 && argv[1][0] != '-' && argv[1][0] != '\0' &&
        (strstr(argv[1], ".wav") != NULL || fileExists(argv[1]));

    if (looksLikeResampler) {
        NoteRequest req;
        std::string inWav = argv[1];
        std::string outWav = argv[2];
        req.note = parseNote(argv[3]);
        req.velocity = atoi(argv[4]);
        req.flags = argv[5];
        req.offsetMs = atof(argv[6]);
        req.lengthMs = atof(argv[7]);
        req.consonantMs = atof(argv[8]);
        // argv[9] is cutoff, argv[10] volume, argv[11] modulation.
        req.tempo = 120.0;
        std::string tempoArg = argv[12];
        if (!tempoArg.empty() && tempoArg[0] == '!') tempoArg = tempoArg.substr(1);
        if (atof(tempoArg.c_str()) > 0) req.tempo = atof(tempoArg.c_str());

        if (!fileExists(inWav)) {
            std::cerr << "vConnect-STAND: input not found: " << inWav << "\n";
            return 1;
        }

        // The corpus identity and the preutterance timing both live in the
        // bank's oto.ini — neither is transmitted by the resampler protocol.
        // Resolve the entry for this sample; without it there is nothing to
        // synthesize from (and no timing to build a sane note around).
        OtoEntry oto;
        std::string otoPath = dirName(inWav) + "/oto.ini";
        if (!findOtoEntry(otoPath, baseName(inWav), oto)) {
            std::cerr << "vConnect-STAND: no oto.ini entry for '"
                      << baseName(inWav) << "' in " << otoPath << "\n";
            return 1;
        }
        std::string otoEncoding = detectOtoEncoding(otoPath);
        req.preutterMs = oto.preutterMs;
        req.overlapMs = oto.overlapMs;
        std::string lyric = oto.alias;

        char tmpl[] = "/tmp/vconnect-vsq-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) {
            std::cerr << "vConnect-STAND: cannot create temp file\n";
            return 1;
        }
        std::string vsqPath = std::string(tmpl) + ".txt";
        close(fd);
        unlink(tmpl);

        std::string text = buildVsq(req, inWav, lyric);
        FILE* f = fopen(vsqPath.c_str(), "wb");
        if (!f) {
            std::cerr << "vConnect-STAND: cannot write " << vsqPath << "\n";
            return 1;
        }
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);

        int rc = runSynthesizer(vsqPath, outWav, otoEncoding);
        // VCONNECT_KEEP_VSQ=1 leaves the generated meta-text behind for debugging.
        if (!getenv("VCONNECT_KEEP_VSQ")) {
            unlink(vsqPath.c_str());
        } else {
            std::cerr << "vConnect-STAND: kept " << vsqPath << "\n";
        }
        return rc;
    }

    // ---- original vConnect CLI ------------------------------------------
    if (argc < 2) {
        std::cout << "v.Connect-STAND ver2.0.0\n"
                  << "usage:\n"
                  << "    vConnect -i [vsq_meta_text_path] -o [out_wave_path] {options}\n"
                  << "    vConnect [vsq_meta_text_path] [out_wave_path]\n"
                  << "      or\n"
                  << "    vConnect -c -i [utau_oto_ini_path] -o [out_stand_path]\n"
                  << "    vConnect -t -i [src_vConnect_ini_path] -o [dst_vConnect_ini_path]\n"
                  << "      or (UTAU resampler protocol)\n";
        printResamplerUsage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        std::cout << "v.Connect-STAND ver2.0.0\n"
                  << "  -i/-o   synthesis (VSQ meta-text -> wave)\n"
                  << "  -c      convert oto.ini to a vConnect library\n"
                  << "  -t      transcribe\n";
        printResamplerUsage(argv[0]);
        return 0;
    }

    vconnect::RuntimeOption option(argc, argv);
    vconnect::Synthesizer synth(option);
    synth.run();
    return 0;
}
