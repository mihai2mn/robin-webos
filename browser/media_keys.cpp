// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_keys.hpp"

namespace browser {
namespace {

// Stable SDL2 scancode numbers (matching wnk/input/input.cpp, SDL-free here for host unit tests).
constexpr int kScancodeSpace = 44;
constexpr int kScancodePause = 72;
constexpr int kScancodeStop = 120;
constexpr int kScancodeAudioStop = 260;
constexpr int kScancodeAudioPlay = 261;
constexpr int kScancodeAudioRewind = 285;
constexpr int kScancodeAudioFastForward = 286;

// Finds the page's main <video> (or <audio> fallback) and invokes `op(v)`.
// Keeps well within WPE_IPC_MAX_TEXT (2048 bytes).
std::string build_script(const char *op_body) {
    // Priority:
    //  1. Video inside document.fullscreenElement (or webkitFullscreenElement).
    //  2. Connected <video> with media across document and same-origin iframes (and open shadow roots if none
    //     found directly), preferring playing non-preview > non-preview > playing > visible > unmuted > controls > area.
    //  3. Connected <audio> with media if no <video> exists.
    static constexpr const char kPrefix[] =
        "(function(){"
        "function pick(ls){"
        "var b=null,bs=-1;"
        "for(var i=0;i<ls.length;i++){"
        "var v=ls[i];if(!v||!v.isConnected)continue;"
        "if(!(v.readyState>0||v.currentSrc||v.src||v.querySelector('source')))continue;"
        "var r=v.getBoundingClientRect(),cs=getComputedStyle(v);"
        "var vis=r.width>0&&r.height>0&&cs.display!=='none'&&cs.visibility!=='hidden';"
        "var area=Math.min(Math.max(0,r.width)*Math.max(0,r.height),5e6);"
        "var prev=v.hasAttribute('wren-software')&&v.muted&&!v.controls;"
        "var pl=!v.paused&&!v.ended;"
        "var s=area+(pl&&!prev?1e11:0)+(!prev?1e9:0)+(pl?1e8:0)+(vis?1e7:0)+(!v.muted?1e6:0)+(v.controls?5e5:0);"
        "if(s>bs){bs=s;b=v;}"
        "}return b;}"
        "var v=null,fs=document.fullscreenElement||document.webkitFullscreenElement;"
        "if(fs)v=fs.tagName==='VIDEO'?fs:pick(fs.querySelectorAll('video'));"
        "if(!v){"
        "var vs=[];"
        "function col(r,sh){"
        "if(!r||!r.querySelectorAll)return;"
        "var a=r.querySelectorAll('video,iframe');"
        "for(var i=0;i<a.length;i++){var e=a[i];if(e.tagName==='VIDEO')vs.push(e);"
        "else{try{if(e.contentDocument)col(e.contentDocument,sh);}catch(x){}}}"
        "if(sh){var n=r.querySelectorAll('*');for(var j=0;j<n.length;j++)if(n[j].shadowRoot)col(n[j].shadowRoot,1);}}"
        "col(document,0);v=pick(vs);"
        "if(!v){vs=[];col(document,1);v=pick(vs);}"
        "if(!v){var au=document.querySelectorAll('audio');for(var k=0;k<au.length;k++){"
        "if(au[k].readyState>0||au[k].currentSrc||au[k].src){if(!v||(!au[k].paused&&!au[k].ended))v=au[k];}}}}"
        "if(!v)return;";
    std::string js;
    js.reserve(sizeof(kPrefix) + 256);
    js.append(kPrefix);
    js.append(op_body);
    js.append("})();");
    return js;
}

}  // namespace

MediaKeyAction media_key_action(wnk::input::Action action, int scancode, int keycode) {
    // Spacebar on a host keyboard maps to Action::PlayPause in host_keymap(), but on a web page Space belongs
    // to the page (scrolling, buttons, page shortcuts), never to this remote-media-key handler.
    if (scancode == kScancodeSpace || keycode == ' ') return MediaKeyAction::None;

    switch (action) {
    case wnk::input::Action::PlayPause:
    case wnk::input::Action::Play:
        return MediaKeyAction::PlayPause;
    case wnk::input::Action::Pause:
        return MediaKeyAction::Pause;
    case wnk::input::Action::Stop:
        return MediaKeyAction::Stop;
    case wnk::input::Action::Rewind:
        return MediaKeyAction::Rewind;
    case wnk::input::Action::FastForward:
        return MediaKeyAction::FastForward;
    default:
        break;
    }

    switch (scancode) {
    case kScancodeAudioPlay:
        return MediaKeyAction::PlayPause;
    case kScancodePause:
        return MediaKeyAction::Pause;
    case kScancodeStop:
    case kScancodeAudioStop:
        return MediaKeyAction::Stop;
    case kScancodeAudioRewind:
        return MediaKeyAction::Rewind;
    case kScancodeAudioFastForward:
        return MediaKeyAction::FastForward;
    default:
        return MediaKeyAction::None;
    }
}

std::string media_key_script(MediaKeyAction cmd) {
    switch (cmd) {
    case MediaKeyAction::PlayPause:
        return build_script("if(v.paused||v.ended){var p=v.play();if(p&&p.catch)p.catch(function(){});}else{v.pause();}");
    case MediaKeyAction::Pause:
        return build_script("v.pause();");
    case MediaKeyAction::Stop:
        return build_script("v.pause();try{v.currentTime=0;}catch(e){}");
    case MediaKeyAction::Rewind:
        return build_script(
            "var t=(v.currentTime||0)-10,lo=0,hi=isFinite(v.duration)&&v.duration>0?v.duration:Infinity;"
            "if(!isFinite(hi)&&v.seekable&&v.seekable.length){lo=v.seekable.start(0);hi=v.seekable.end(v.seekable.length-1);}"
            "if(t<lo)t=lo;if(t>hi)t=hi;try{v.currentTime=t;}catch(e){}");
    case MediaKeyAction::FastForward:
        return build_script(
            "var t=(v.currentTime||0)+10,lo=0,hi=isFinite(v.duration)&&v.duration>0?v.duration:Infinity;"
            "if(!isFinite(hi)&&v.seekable&&v.seekable.length){lo=v.seekable.start(0);hi=v.seekable.end(v.seekable.length-1);}"
            "if(t<lo)t=lo;if(t>hi)t=hi;try{v.currentTime=t;}catch(e){}");
    case MediaKeyAction::None:
        return {};
    }
    return {};
}

const char *media_key_label(MediaKeyAction cmd) {
    switch (cmd) {
    case MediaKeyAction::PlayPause: return "Play / Pause";
    case MediaKeyAction::Pause: return "Pause";
    case MediaKeyAction::Stop: return "Stop";
    case MediaKeyAction::Rewind: return "-10 s";
    case MediaKeyAction::FastForward: return "+10 s";
    case MediaKeyAction::None: return "";
    }
    return "";
}

}  // namespace browser
