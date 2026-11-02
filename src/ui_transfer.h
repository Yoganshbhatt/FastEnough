#pragma once
#include "ui_widgets.h"
#include <string>

namespace ui {
enum class TransferTone { Active, Attention, Success, Error, Neutral };
enum class TransferCardAction { None, PauseResume, Cancel, Retry, Review, Details, Channels };

struct TransferCardModel {
    std::string heading, filename, route, amount, metric, eta, detail;
    float progress = 0;
    TransferTone tone = TransferTone::Active;
    bool indeterminate = false;
    bool paused = false;
    bool canPause = false;
    bool canCancel = false;
    bool canRetry = false;
    bool canReview = false;
    bool showChannels = false;
    int queued = 0;
};

inline bool transferCardCompact(float width) {
    return width < std::max(740.0f * scale(), 56.0f * ImGui::GetFontSize());
}

inline float transferCardHeight(const TransferCardModel& model, float width) {
    const float s = scale(), f = ImGui::GetFontSize();
    const float header = std::max(42*s, f*2.25f+4*s);
    const float body = std::max(34*s, f*2.0f+4*s);
    const float footer = controlHeight(28);
    const bool hasControls = model.canPause || model.canRetry || model.canReview || model.canCancel;
    return 32*s+header+10*s+body+10*s+12*s+6*s+footer+
        (hasControls && transferCardCompact(width) ? controlHeight(36)+10*s : 0);
}

inline ImVec4 transferToneColor(TransferTone tone, bool light) {
    switch (tone) {
    case TransferTone::Attention: return light ? ImVec4(0.60f,0.33f,0.02f,1) : ImVec4(1,0.73f,0.29f,1);
    case TransferTone::Success: return light ? ImVec4(0.08f,0.43f,0.29f,1) : ImVec4(0.32f,0.87f,0.64f,1);
    case TransferTone::Error: return light ? ImVec4(0.73f,0.17f,0.21f,1) : ImVec4(1,0.48f,0.49f,1);
    case TransferTone::Neutral: return light ? ImVec4(0.35f,0.41f,0.50f,1) : ImVec4(0.68f,0.74f,0.84f,1);
    default: return light ? ImVec4(0.13f,0.37f,0.79f,1) : ImVec4(0.39f,0.65f,1,1);
    }
}

inline TransferCardAction transferCard(const TransferCardModel& model, float width) {
    TransferCardAction action = TransferCardAction::None;
    const float s = scale(), f = ImGui::GetFontSize(), p = 16*s;
    const float height = transferCardHeight(model, width);
    const bool compact = transferCardCompact(width);
    const auto& style = ImGui::GetStyle();
    const bool light = style.Colors[ImGuiCol_WindowBg].x > 0.5f;
    const ImVec4 accent = transferToneColor(model.tone, light);
    const ImVec4 surface = light ? ImVec4(0.96f,0.98f,1,1) : ImVec4(0.066f,0.093f,0.14f,1);
    const ImU32 ink = ImGui::GetColorU32(light ? ImVec4(0.10f,0.16f,0.25f,1) : ImVec4(0.94f,0.96f,1,1));
    const ImU32 secondary = ImGui::GetColorU32(light ? ImVec4(0.34f,0.40f,0.49f,1) : ImVec4(0.64f,0.72f,0.83f,1));
    const ImU32 accentInk = ImGui::GetColorU32(accent);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(p,p));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12*s);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1*s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7*s);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, surface);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x,accent.y,accent.z,light?0.26f:0.32f));
    ImGui::BeginChild("##TransferCard", ImVec2(width,height), ImGuiChildFlags_Borders,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(origin.x+1*s,origin.y+12*s),
        ImVec2(origin.x+5*s,origin.y+height-12*s), accentInk, 2*s);
    const float contentWidth = std::min(width-2*p, 1400*s);
    const float left = origin.x+(width-contentWidth)*0.5f;
    const float right = left+contentWidth;
    const float top = origin.y+p;
    const float headerH = std::max(42*s, f*2.25f+4*s);
    const float bodyH = std::max(34*s, f*2.0f+4*s);
    const float buttonH = controlHeight(36);
    auto buttonWidth = [&](const char* label) { return ImGui::CalcTextSize(label).x+40*s; };
    const char* primary = model.canReview ? "Review decision" : model.canRetry ? "Retry now" : model.paused ? "Resume" : "Pause";
    const bool hasPrimary = model.canReview || model.canRetry || model.canPause;
    const float primaryW = hasPrimary ? buttonWidth(primary) : 0;
    const float cancelW = model.canCancel ? buttonWidth("Cancel") : 0;
    const float controlsWidth = primaryW+cancelW+(hasPrimary && model.canCancel ? 8*s : 0);
    const float titleSize = f*1.25f;
    const float iconSize = 24*s;
    const float iconBox = std::max(40*s,headerH-2*s);
    const ImVec2 iconPos(left,top);
    draw->AddRectFilled(iconPos,ImVec2(left+iconBox,top+iconBox),
        ImGui::GetColorU32(ImVec4(accent.x,accent.y,accent.z,light?0.11f:0.13f)),9*s);
    Icon glyph = model.tone == TransferTone::Success ? Icon::Check :
        model.tone == TransferTone::Attention || model.tone == TransferTone::Error ? Icon::Info :
        model.paused ? Icon::Pause : Icon::Transfer;
    icon(glyph,ImVec2(left+(iconBox-iconSize)*0.5f,top+(iconBox-iconSize)*0.5f),iconSize,accentInk);
    const float titleX = left+iconBox+12*s;
    const float titleWidth = right-titleX-(compact || controlsWidth==0 ? 0 : controlsWidth+20*s);
    ImFont* titleFont = semiboldFont ? semiboldFont : ImGui::GetFont();
    draw->PushClipRect(ImVec2(titleX,top),ImVec2(titleX+titleWidth,top+headerH),true);
    draw->AddText(titleFont,titleSize,ImVec2(titleX,top),accentInk,model.heading.c_str());
    draw->PopClipRect();
    drawTextClipped(ImVec2(titleX,top+titleSize+4*s),model.route,titleWidth,secondary);
    ImGui::SetCursorScreenPos(ImVec2(titleX,top+titleSize+4*s));
    ImGui::Dummy(ImVec2(titleWidth,f));
    tooltip(model.route.c_str());

    auto controls = [&](float x,float y) {
        ImGui::PushStyleColor(ImGuiCol_CheckMark,accent);
        if (hasPrimary) {
            ImGui::SetCursorScreenPos(ImVec2(x,y));
            const Icon primaryIcon = model.canReview ? Icon::Info : model.canRetry ? Icon::Refresh : model.paused ? Icon::Play : Icon::Pause;
            if (button(primary,primaryIcon,model.canReview||model.canRetry?Appearance::Primary:Appearance::Secondary,ImVec2(primaryW,buttonH)))
                action = model.canReview ? TransferCardAction::Review : model.canRetry ? TransferCardAction::Retry : TransferCardAction::PauseResume;
            x+=primaryW+8*s;
        }
        if (model.canCancel) {
            ImGui::SetCursorScreenPos(ImVec2(x,y));
            if (button("Cancel",Icon::Close,Appearance::Secondary,ImVec2(cancelW,buttonH))) action=TransferCardAction::Cancel;
        }
        ImGui::PopStyleColor();
    };
    if (!compact) controls(right-controlsWidth,top+std::max(0.0f,(headerH-buttonH)*0.5f));

    const float bodyY = top+headerH+10*s;
    const float metricSize = f*1.15f;
    float metricWidth = std::max(ImGui::GetFont()->CalcTextSizeA(metricSize,FLT_MAX,0,model.metric.c_str()).x,
        ImGui::CalcTextSize(model.eta.c_str()).x);
    metricWidth = std::min(metricWidth,contentWidth*0.42f);
    const float nameWidth = contentWidth-(metricWidth>0?metricWidth+20*s:0);
    drawTextClipped(ImVec2(left,bodyY),model.filename,nameWidth,ink,f*1.1f);
    drawTextClipped(ImVec2(left,bodyY+f*1.1f+4*s),model.detail,nameWidth,secondary,f*0.9f);
    if (metricWidth>0) {
        drawTextClipped(ImVec2(right-metricWidth,bodyY),model.metric,metricWidth,ink,metricSize);
        drawTextClipped(ImVec2(right-metricWidth,bodyY+metricSize+4*s),model.eta,metricWidth,secondary,f*0.9f);
    }
    ImGui::SetCursorScreenPos(ImVec2(left,bodyY));
    ImGui::Dummy(ImVec2(nameWidth,bodyH));
    tooltip(model.filename.c_str());

    const float trackY = bodyY+bodyH+10*s;
    const float trackH = 12*s;
    draw->AddRectFilled(ImVec2(left,trackY),ImVec2(right,trackY+trackH),
        ImGui::GetColorU32(light?ImVec4(0.85f,0.89f,0.94f,1):ImVec4(0.13f,0.18f,0.25f,1)),6*s);
    const float fraction = std::clamp(model.progress,0.0f,1.0f);
    if (model.indeterminate) {
        const float length = contentWidth*0.18f;
        const float travel = float(std::fmod(ImGui::GetTime()*0.45,1.0))*(contentWidth+length)-length;
        draw->PushClipRect(ImVec2(left,trackY),ImVec2(right,trackY+trackH),true);
        draw->AddRectFilled(ImVec2(left+travel,trackY),ImVec2(left+travel+length,trackY+trackH),accentInk,6*s);
        draw->PopClipRect();
    } else if (fraction>0) {
        draw->AddRectFilled(ImVec2(left,trackY),ImVec2(left+std::max(trackH,contentWidth*fraction),trackY+trackH),accentInk,6*s);
    }
    const float footerY = trackY+trackH+6*s;
    std::string amount = model.amount;
    if (!model.indeterminate) amount += "   " + std::to_string((int)std::round(fraction*100)) + "%";
    if (model.queued>0) amount+="   "+std::to_string(model.queued)+" queued";
    const float footerH = controlHeight(28);
    const float detailsW = ImGui::CalcTextSize("Details").x+38*s;
    const float channelsW = 30*s;
    drawTextClipped(ImVec2(left,footerY+(footerH-f)*0.5f),amount,contentWidth-detailsW-channelsW-12*s,ink);
    ImGui::SetCursorScreenPos(ImVec2(right-detailsW-channelsW-4*s,footerY));
    if (button("Details",Icon::Info,Appearance::Quiet,ImVec2(detailsW,footerH))) action=TransferCardAction::Details;
    ImGui::SetCursorScreenPos(ImVec2(right-channelsW,footerY));
    if (button("##Channels",Icon::Settings,model.showChannels?Appearance::Selected:Appearance::Quiet,
        ImVec2(channelsW,footerH),"Connection speeds and queued transfers")) action=TransferCardAction::Channels;
    if (compact) controls(left,footerY+footerH+10*s);
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
    return action;
}
}
