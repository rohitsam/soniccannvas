#pragma once
// Root-page HTML/JS for the SonicCanvas web control interface.
// Call buildRootHTML() to get the full page string; send it in the "/" route handler.

#include <Arduino.h>
#include <WiFi.h>
#include "panel_defs.h"
#include "bt_audio.h"
#include "visualizer.h"

String buildRootHTML() {
    String html =
      F("<!DOCTYPE html><html><head><style>"
        ".ef{position:absolute;pointer-events:none;user-select:none;font-size:40px;line-height:1;background:transparent}"
        ".rb{font-size:1.9em;padding:6px 11px;border:1px solid #333;border-radius:12px;"
            "cursor:pointer;background:#1a1a1a;line-height:1;transition:transform .1s;"
            "outline:none;-webkit-tap-highlight-color:transparent}"
        ".rb:active{transform:scale(.85)}"
        "</style></head>"
        "<body style='font-family:sans-serif;max-width:520px;margin:20px auto'>"
        "<h2>SonicCanvas</h2>"
        "<p><b>Visualizer:</b> <span id='vn' style='color:#0a7'></span></p>"
        "<div id='vmd' style='display:flex;flex-wrap:wrap;gap:5px;margin-bottom:14px'></div>");

    // UDP status banner
    html += F("<p style='background:#111;color:#fff;padding:6px;border-radius:4px'>&#127911; UDP audio: ");
    if (btIsConnected()) {
        html += F("<span style='color:#0f0'>&#9679; Connected — ");
        html += bt_peer;
        html += "</span>";
    } else {
        html += F("<span style='color:#fa0'>&#9679; Idle — send raw PCM to port 4210</span>");
    }
    html += F("</p>"
              "<p style='font-size:.8em;color:#555;margin-top:0'>Stream command:<br>"
              "<code style='background:#eee;padding:2px 4px'>"
              "ffmpeg -f dshow -audio_buffer_size 50 -i audio=\"Stereo Mix\" "
              "-ar 44100 -ac 1 -f s16le -flush_packets 1 \"udp://");
    html += WiFi.localIP().toString();
    html += F(":4210?pkt_size=882\"</code></p>");

    // Panel count selectors
    {
        int actPanels = SD_W / PANEL_RES_X;
        html += F("<p><b>Active panels</b> (visualization): ");
        html += String(actPanels);
        html += F(" &nbsp;<select id='pc'>");
        for (int i = 1; i <= 6; i++) {
            html += "<option value='" + String(i) + "'" +
                    (i == actPanels ? " selected" : "") + ">" +
                    String(i) + " panels (" + String(i*PANEL_RES_X) + " px)</option>";
        }
        html += F("</select></p>"
                  "<p><b>Physical chain</b> (all connected panels): ");
        html += String(g_max_panels);
        html += F(" &nbsp;<select id='mx'>");
        for (int i = 1; i <= 6; i++) {
            html += "<option value='" + String(i) + "'" +
                    (i == g_max_panels ? " selected" : "") + ">" +
                    String(i) + " panels (" + String(i*PANEL_RES_X) + " px)</option>";
        }
        html += F("</select>"
                  " <button onclick=\""
                    "var n=parseInt(document.getElementById('pc').value),"
                    "mx=parseInt(document.getElementById('mx').value);"
                    "if(mx<n){alert('Physical chain must be ≥ active panels.');return;}"
                    "if(confirm('Save and reboot?'))"
                      "fetch('/panels?n='+n+'&max='+mx)"
                        ".then(function(){setTimeout(function(){location.reload();},3500);})"
                  "\">Apply &amp; Reboot</button></p>");
    }

    // Emoji reactions
    html += F("<p style='font-size:.72em;color:#777;margin:12px 0 4px'>"
              "REACTIONS</p>"
              "<div id='ra' style='position:relative;overflow:hidden;height:120px;"
              "border-radius:8px;background:#0c0c0c;margin-bottom:10px'>"
              "<div id='ro' style='position:absolute;inset:0;pointer-events:none;"
              "overflow:hidden'></div>"
              "<div id='rb' style='position:relative;z-index:1;display:flex;"
              "flex-wrap:wrap;justify-content:center;gap:8px;padding:12px'>"
              "</div>\xF0\x9F\x8E\xB5</div>");

    // Breathing light section
    html += F("<hr style='border:0;border-top:1px solid #1a1a1a;margin:14px 0'>"
              "<p style='font-size:.72em;color:#777;margin:8px 0 2px'>"
              "BREATHING &mdash; 4 &middot; 7 &middot; 8</p>"
              "<p style='font-size:.75em;color:#444;margin:0 0 8px'>"
              "Inhale 4s &bull; Hold 7s &bull; Exhale 8s</p>"
              "<div style='display:flex;align-items:center;gap:10px;flex-wrap:wrap;"
                          "margin-bottom:14px'>"
              "<button id='bzb' type='button' onclick='startBreathe()'"
                      " style='font-size:1em;padding:8px 18px;border:none;border-radius:8px;"
                              "background:#0d3a50;color:#7cf;cursor:pointer;outline:none;"
                              "-webkit-tap-highlight-color:transparent'>Breathe 4-7-8</button>"
              "<button id='bzx' type='button' onclick='stopBreathe()'"
                      " style='font-size:1em;padding:8px 18px;border:none;border-radius:8px;"
                              "background:#222;color:#888;cursor:pointer;outline:none;"
                              "-webkit-tap-highlight-color:transparent;display:none'>Stop</button>"
              "<span id='bzp' style='font-size:1.2em;color:#7cf;"
                                    "letter-spacing:1px;display:none'></span>"
              "</div>");

    // Color mixer section
    html += F("<hr style='border:0;border-top:1px solid #1a1a1a;margin:14px 0'>"
              "<p style='font-size:.72em;color:#777;margin:8px 0 6px'>COLOR MIX</p>"
              "<div style='margin-bottom:6px'>"
                "<div style='display:flex;align-items:center;gap:8px;margin-bottom:5px'>"
                  "<b style='color:#f66;min-width:12px'>R</b>"
                  "<input type='range' id='mr' min='0' max='255' value='0'"
                         " oninput='mixChange()' style='flex:1;accent-color:#f55'>"
                  "<span id='mrv' style='width:26px;text-align:right;font-size:.8em;color:#555'>0</span>"
                "</div>"
                "<div style='display:flex;align-items:center;gap:8px;margin-bottom:5px'>"
                  "<b style='color:#4d4;min-width:12px'>G</b>"
                  "<input type='range' id='mg' min='0' max='255' value='128'"
                         " oninput='mixChange()' style='flex:1;accent-color:#4d4'>"
                  "<span id='mgv' style='width:26px;text-align:right;font-size:.8em;color:#555'>128</span>"
                "</div>"
                "<div style='display:flex;align-items:center;gap:8px;margin-bottom:10px'>"
                  "<b style='color:#56f;min-width:12px'>B</b>"
                  "<input type='range' id='mb' min='0' max='255' value='160'"
                         " oninput='mixChange()' style='flex:1;accent-color:#56f'>"
                  "<span id='mbv' style='width:26px;text-align:right;font-size:.8em;color:#555'>160</span>"
                "</div>"
              "</div>"
              "<div style='display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:14px'>"
                "<div id='mcp' style='width:52px;height:34px;border-radius:6px;"
                                     "background:rgb(0,128,160);border:1px solid #333'></div>"
                "<code id='mch' style='font-size:.82em;color:#555'>#0080A0</code>"
                "<button id='mcb' type='button' onclick='startMix()'"
                        " style='font-size:1em;padding:7px 16px;border:none;border-radius:8px;"
                                "background:#1a1a2e;color:#a8c;cursor:pointer;outline:none;"
                                "-webkit-tap-highlight-color:transparent'>Color Mix</button>"
                "<button id='mcx' type='button' onclick='stopMix()'"
                        " style='font-size:1em;padding:7px 16px;border:none;border-radius:8px;"
                                "background:#222;color:#888;cursor:pointer;outline:none;"
                                "-webkit-tap-highlight-color:transparent;display:none'>Stop</button>"
              "</div>");

    html += F("<p><a href='/fs'>&#128193; Files</a> &nbsp; <a href='/update'>&#8593; OTA</a></p>"
              "<script>");
    html += "var curViz=" + String(g_viz_mode) + ";";
    html +=
      F("var vnm=['Spectrum','Mirror','Waterfall','Color Organ','Oscilloscope','Echo Wave','Fire','VU Meter','Beat Flash','Plasma','Starfield','Breathing','Color Mix'];"
        "function setViz(n){"
        "  fetch('/viz?n='+n).then(function(){"
        "    curViz=n;"
        "    document.getElementById('vn').textContent=vnm[n]||'';"
        "    document.querySelectorAll('.vb').forEach(function(b,i){"
        "      b.style.background=i===n?'#0a7':'#444';"
        "    });"
        "  });"
        "}"
        "(function(){"
        "  var vd=document.getElementById('vmd');"
        "  vnm.forEach(function(nm,i){"
        "    if(i>=11)return;"
        "    var b=document.createElement('button');b.textContent=nm;b.className='vb';"
        "    b.style.cssText='padding:5px 10px;border:none;border-radius:4px;cursor:pointer;"
                             "color:#fff;font-size:.82em;background:'+(i===curViz?'#0a7':'#444');"
        "    b.onclick=function(){setViz(i);};"
        "    vd.appendChild(b);"
        "  });"
        "  document.getElementById('vn').textContent=vnm[curViz]||'Breathing';"
        "})();"
        // Emoji reaction JS
        "function spawnEmoji(ch){"
          "var ro=document.getElementById('ro'),"
              "h=ro.getBoundingClientRect().height||120,"
              "w=ro.getBoundingClientRect().width||480;"
          "var el=document.createElement('span');"
          "el.className='ef';"
          "el.textContent=ch;"
          "el.style.left=(8+Math.random()*72)+'%';"
          "el.style.bottom='4px';"
          "ro.appendChild(el);"
          "var dx=(Math.random()-.5)*Math.min(w*.18,80),"
              "r0=(Math.random()-.5)*28,r1=(Math.random()-.5)*28,"
              "tr=h+50,dur=2100+Math.random()*900;"
          "el.animate(["
            "{transform:'translateY(0) scale(.3) rotate('+r0+'deg)',opacity:0},"
            "{transform:'translateY(-'+(tr*.13)+'px) scale(1.3) rotate(0deg)',"
             "opacity:1,offset:.13},"
            "{transform:'translateY(-'+tr+'px) translateX('+dx+'px) scale(.85) rotate('+r1+'deg)',"
             "opacity:0}"
          "],{duration:dur,easing:'ease-out',fill:'forwards'})"
          ".onfinish=function(){el.remove();};"
        "}"
        "function react(ch,code){"
          "spawnEmoji(ch);"
          "fetch('/text?msg='+code+'&once=1');"
        "}"
        "(function(){"
          "var REACTS=["
            "['\\uD83C\\uDFB5','%01'],"
            "['\\u2764\\uFE0F','%02'],"
            "['\\u2B50','%03'],"
            "['\\u26A1','%04'],"
            "['\\uD83D\\uDE0A','%05'],"
            "['\\uD83C\\uDF19','%06'],"
            "['\\uD83D\\uDD25','%07'],"
            "['\\uD83C\\uDFB6','%08']"
          "];"
          "var rb=document.getElementById('rb');"
          "REACTS.forEach(function(r){"
            "var b=document.createElement('button');"
            "b.type='button';"
            "b.className='rb';"
            "b.textContent=r[0];"
            "b.onclick=function(){"
              "var now=Date.now();"
              "if(now-(this._t||0)<800)return;"
              "this._t=now;"
              "react(r[0],r[1]);"
            "};"
            "rb.appendChild(b);"
          "});"
        "})();"
        // Breathing JS
        "var BZ_INT=null,BZ_T0=0,BZ_PV=0;"
        "function startBreathe(){"
          "BZ_PV=curViz;curViz=11;BZ_T0=Date.now();"
          "fetch('/breathe');"
          "document.getElementById('bzb').style.display='none';"
          "document.getElementById('bzx').style.display='';"
          "document.getElementById('bzp').style.display='';"
          "document.getElementById('vn').textContent='Breathing';"
          "if(BZ_INT)clearInterval(BZ_INT);"
          "BZ_INT=setInterval(function(){"
            "var t=(Date.now()-BZ_T0)%19000,ph,s;"
            "if(t<4000){ph='Inhale ↑';s=Math.ceil((4000-t)/1000);}"
            "else if(t<11000){ph='Hold ―';s=Math.ceil((11000-t)/1000);}"
            "else{ph='Exhale ↓';s=Math.ceil((19000-t)/1000);}"
            "document.getElementById('bzp').textContent=ph+' '+s+'s';"
          "},200);"
        "}"
        "function stopBreathe(){"
          "clearInterval(BZ_INT);BZ_INT=null;"
          "fetch('/viz?n='+BZ_PV);"
          "curViz=BZ_PV;"
          "document.getElementById('bzb').style.display='';"
          "document.getElementById('bzx').style.display='none';"
          "document.getElementById('bzp').style.display='none';"
          "document.getElementById('vn').textContent=vnm[BZ_PV]||'';"
        "}"
        // Color mixer JS
        "var MC_TIMER=null,MC_ACTIVE=false,MC_PV=0;"
        "function mixChange(){"
          "var r=document.getElementById('mr').value,"
              "g=document.getElementById('mg').value,"
              "b=document.getElementById('mb').value;"
          "document.getElementById('mcp').style.background='rgb('+r+','+g+','+b+')';"
          "var h2=function(n){return('0'+parseInt(n).toString(16)).slice(-2);};"
          "document.getElementById('mch').textContent='#'+h2(r)+h2(g)+h2(b);"
          "document.getElementById('mrv').textContent=r;"
          "document.getElementById('mgv').textContent=g;"
          "document.getElementById('mbv').textContent=b;"
          "if(!MC_ACTIVE)startMix();"
          "clearTimeout(MC_TIMER);"
          "MC_TIMER=setTimeout(function(){fetch('/mix?r='+r+'&g='+g+'&b='+b);},30);"
        "}"
        "function startMix(){"
          "if(MC_ACTIVE)return;"
          "MC_ACTIVE=true;MC_PV=curViz;curViz=12;"
          "fetch('/colormix');"
          "document.getElementById('mcb').style.display='none';"
          "document.getElementById('mcx').style.display='';"
          "document.getElementById('vn').textContent='Color Mix';"
        "}"
        "function stopMix(){"
          "MC_ACTIVE=false;"
          "fetch('/viz?n='+MC_PV);"
          "curViz=MC_PV;"
          "document.getElementById('mcb').style.display='';"
          "document.getElementById('mcx').style.display='none';"
          "document.getElementById('vn').textContent=vnm[MC_PV]||'';"
        "}"
        "</script></body></html>");

    return html;
}
