// Text-to-speech runtime plugin. Scripts of a project that enables "tts" get:
//   tts.speak(text, {lang, rate, pitch}) -> bool   queue an utterance
//   tts.stop()                                      cancel the current one and the queue
//   tts.available() -> bool                         a voice can speak here
// Windows uses SAPI (installed system voices), the Web player the browser's
// speechSynthesis. Other platforms report unavailable.

#include "plugins/EnginePlugin.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sapi.h>
#endif

namespace {

struct Utterance {
    std::string text;
    std::string lang = "fr-FR";
    float rate = 1.0f;
    float pitch = 1.0f;
};

#if defined(__EMSCRIPTEN__)

EM_JS(int, ttsWebSpeak, (const char* text, const char* lang, float rate, float pitch), {
    if (typeof speechSynthesis === 'undefined') return 0;
    const l = UTF8ToString(lang);
    const u = new SpeechSynthesisUtterance(UTF8ToString(text));
    u.lang = l;
    u.rate = rate;
    u.pitch = pitch;
    const v = speechSynthesis.getVoices().find(x => x.lang && x.lang.replace('_', '-').startsWith(l.slice(0, 2)));
    if (v) u.voice = v;
    speechSynthesis.speak(u);
    return 1;
});
EM_JS(void, ttsWebStop, (), {
    if (typeof speechSynthesis !== 'undefined') speechSynthesis.cancel();
});
EM_JS(int, ttsWebAvailable, (), { return typeof speechSynthesis !== 'undefined' ? 1 : 0; });

class Speaker {
public:
    bool speak(const Utterance& u) { return ttsWebSpeak(u.text.c_str(), u.lang.c_str(), u.rate, u.pitch) != 0; }
    void stop() { ttsWebStop(); }
    bool available() { return ttsWebAvailable() != 0; }
    void release() { stop(); }
};

#elif defined(_WIN32)

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring escapeXml(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'&') out += L"&amp;";
        else if (c == L'<') out += L"&lt;";
        else if (c == L'>') out += L"&gt;";
        else out += c;
    }
    return out;
}

// SAPI is single-threaded COM, created lazily on the thread that runs scripts.
class Speaker {
public:
    bool speak(const Utterance& u) {
        ISpVoice* v = voice(u.lang);
        if (!v) return false;
        // SAPI rate is -10..10 where 10 is about three times faster.
        const long rate = std::lround(10.0 * std::log(std::max(0.2f, u.rate)) / std::log(3.0));
        v->SetRate(std::max(-10L, std::min(10L, rate)));
        const long pitch = std::lround((u.pitch - 1.0f) * 20.0f);
        const std::wstring xml = L"<pitch absmiddle=\"" + std::to_wstring(std::max(-10L, std::min(10L, pitch)))
            + L"\">" + escapeXml(widen(u.text)) + L"</pitch>";
        return SUCCEEDED(v->Speak(xml.c_str(), SPF_ASYNC | SPF_IS_XML, nullptr));
    }
    void stop() {
        if (voice_) voice_->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
    }
    bool available() { return voice("") != nullptr; }
    void release() {
        stop();
        if (voice_) voice_->Release();
        voice_ = nullptr;
        if (comInitialized_) CoUninitialize();
        comInitialized_ = false;
        tried_ = false;
    }

private:
    ISpVoice* voice(const std::string& lang) {
        if (!tried_) {
            tried_ = true;
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            comInitialized_ = SUCCEEDED(hr);
            if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                        reinterpret_cast<void**>(&voice_)))) {
                voice_ = nullptr;
                saida::Log::warn("tts: no SAPI voice available");
            }
        }
        if (voice_ && !lang.empty() && lang != lang_) {
            lang_ = lang;
            selectLanguage(lang);
        }
        return voice_;
    }

    // Prefers an installed voice of `lang`; the default voice stays otherwise.
    void selectLanguage(const std::string& lang) {
        const LCID lcid = LocaleNameToLCID(widen(lang).c_str(), 0);
        if (!lcid) return;
        ISpObjectTokenCategory* category = nullptr;
        if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                    IID_ISpObjectTokenCategory, reinterpret_cast<void**>(&category))))
            return;
        IEnumSpObjectTokens* tokens = nullptr;
        wchar_t filter[32];
        swprintf(filter, 32, L"Language=%X", static_cast<unsigned>(LANGIDFROMLCID(lcid)));
        if (SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE)) &&
            SUCCEEDED(category->EnumTokens(filter, nullptr, &tokens)) && tokens) {
            ISpObjectToken* token = nullptr;
            ULONG fetched = 0;
            if (tokens->Next(1, &token, &fetched) == S_OK && token) {
                voice_->SetVoice(token);
                token->Release();
            }
            tokens->Release();
        }
        category->Release();
    }

    ISpVoice* voice_ = nullptr;
    bool tried_ = false;
    bool comInitialized_ = false;
    std::string lang_;
};

#else

class Speaker {
public:
    bool speak(const Utterance&) { return false; }
    void stop() {}
    bool available() { return false; }
    void release() {}
};

#endif

Speaker& speaker() {
    static Speaker instance;
    return instance;
}

JSValue jsSpeak(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "tts.speak(text, {lang, rate, pitch}?)");
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    Utterance u;
    u.text = text;
    JS_FreeCString(ctx, text);
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue lang = JS_GetPropertyStr(ctx, argv[1], "lang");
        if (JS_IsString(lang)) {
            const char* l = JS_ToCString(ctx, lang);
            if (l) { u.lang = l; JS_FreeCString(ctx, l); }
        }
        JS_FreeValue(ctx, lang);
        double value = 0;
        JSValue rate = JS_GetPropertyStr(ctx, argv[1], "rate");
        if (JS_IsNumber(rate) && JS_ToFloat64(ctx, &value, rate) == 0) u.rate = static_cast<float>(value);
        JS_FreeValue(ctx, rate);
        JSValue pitch = JS_GetPropertyStr(ctx, argv[1], "pitch");
        if (JS_IsNumber(pitch) && JS_ToFloat64(ctx, &value, pitch) == 0) u.pitch = static_cast<float>(value);
        JS_FreeValue(ctx, pitch);
    }
    if (u.text.empty()) return JS_NewBool(ctx, false);
    return JS_NewBool(ctx, speaker().speak(u));
}

JSValue jsStop(JSContext*, JSValueConst, int, JSValueConst*) {
    speaker().stop();
    return JS_UNDEFINED;
}

JSValue jsAvailable(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, speaker().available());
}

class TtsPlugin final : public saida::EnginePlugin {
public:
    const char* name() const override { return "tts"; }
    void onDeactivate() override { speaker().release(); }
    void installScriptApi(JSContext* ctx, JSValueConst global) override {
        JSValue tts = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, tts, "speak", JS_NewCFunction(ctx, jsSpeak, "speak", 2));
        JS_SetPropertyStr(ctx, tts, "stop", JS_NewCFunction(ctx, jsStop, "stop", 0));
        JS_SetPropertyStr(ctx, tts, "available", JS_NewCFunction(ctx, jsAvailable, "available", 0));
        JS_SetPropertyStr(ctx, global, "tts", tts);
    }
};

} // namespace

SAIDA_REGISTER_PLUGIN(TtsPlugin)
