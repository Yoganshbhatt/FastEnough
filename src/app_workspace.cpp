#include "app.h"
#include "ui_widgets.h"
#include "ui_transfer.h"
#include "device_dokan.h"
#include "mcraw_projfs.h"
#include <algorithm>
#include <cctype>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

namespace {
bool finished(BatchState state) {
    return state == BatchState::Completed || state == BatchState::Failed || state == BatchState::Stopped;
}

template<class Batches>
std::shared_ptr<TransferBatch> visibleTransfer(const Batches& batches) {
    for (const auto& batch : batches)
        if (!finished(batch->state.load()) && batch->waitingConflict && !batch->stopRequested) return batch;
    for (const auto& batch : batches) {
        const auto state = batch->state.load();
        if (state == BatchState::Running || state == BatchState::Paused ||
            state == BatchState::Verifying || state == BatchState::WaitingConflict) return batch;
    }
    for (const auto& batch : batches)
        if (!finished(batch->state.load())) return batch;
    return batches.empty() ? nullptr : batches.back();
}

const char* batchStateLabel(const TransferBatch& batch) {
    auto state = batch.state.load();
    if (!finished(state) && batch.disconnected) return "Waiting for phone";
    if (batch.waitingConflict) return "Needs a decision";
    switch (state) {
    case BatchState::Preparing: return "Scanning files";
    case BatchState::Queued: return "Queued";
    case BatchState::Running: return "Copying";
    case BatchState::Paused: return "Paused";
    case BatchState::Verifying: return "Checking files";
    case BatchState::Completed:
        return batch.errorSkippedFiles > 0 || batch.crcFailCount() > 0 ? "Completed with errors" : "Completed";
    case BatchState::Failed: return "Failed";
    case BatchState::Stopped: return "Cancelled";
    default: return "Needs a decision";
    }
}

std::string parentPath(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(0, pos);
}

std::wstring workspaceWide(const std::string& value) {
    int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), count);
    return result;
}
}

std::string App::deviceDisplayName(int slot) const {
    auto found = m_deviceDisplayNames.find(deviceSession(slot).serial);
    if (found != m_deviceDisplayNames.end() && !found->second.empty()) return found->second;
    if (!deviceSession(slot).serial.empty()) return deviceSession(slot).serial;
    return "Android device";
}

std::string App::panelDisplayName(const FilePanel& panel) const {
    if (panel.isConnections) return "Connections";
    if (!panel.pendingDeviceSerial.empty()) return panel.pendingDeviceName;
    return panel.isAndroid || panel.isApps ? deviceDisplayName(panel.deviceSlot) : "This PC";
}

bool App::panelAvailable(const FilePanel& panel) const {
    if (panel.isConnections || !panel.pendingDeviceSerial.empty()) return false;
    return !panel.isAndroid && !panel.isApps ||
        (deviceSession(panel.deviceSlot).connected && !deviceSession(panel.deviceSlot).rootRequestPending &&
            deviceSession(panel.deviceSlot).client.isServerRunning());
}

void App::renderDeviceRootButton(int slot, FilePanel* panel) {
    auto& session = deviceSession(slot);
    bool pending = session.rootRequestPending;
    bool root = session.connected && session.client.useRoot();
    bool canChange = canChangeDeviceRoot(slot);
    ImGui::BeginDisabled(pending || (!root && !canChange));
    const char* hint = pending ? "Approve the root request on your phone." :
        root ? "Root access is active. Browse the filesystem or disable root." :
        canChange ? "Request root access on this device and open /. The phone must already be rooted." :
        "Connect the device, finish its transfers and unmount its drive before changing root access.";
    if (ui::button(pending ? "ROOT..." : root ? "ROOT ON" : "ROOT", ui::Icon::None,
            root ? ui::Appearance::Selected : ui::Appearance::Secondary, ImVec2(0, ui::controlHeight()), hint)) {
        if (root) ImGui::OpenPopup("##RootActions");
        else requestDeviceRoot(slot, true, panel);
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##RootActions")) {
        ImGui::TextUnformatted("Root access is active");
        ImGui::Separator();
        if (panel && ImGui::MenuItem("Open filesystem /")) navigateToDirectory(*panel, "/");
        if (ImGui::MenuItem("Disable root access", nullptr, false, canChange)) requestDeviceRoot(slot, false);
        ImGui::EndPopup();
    }
}

void App::renderWorkspaceHeader() {
    const float s = ui::scale();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16*s,8*s));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("##WorkspaceHeader", ImVec2(0,ui::controlHeight()+16*s), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_AlwaysUseWindowPadding);
    ui::button("F##Brand", ui::Icon::None, ui::Appearance::Selected, ImVec2(28*s,32*s));
    if (ImGui::GetWindowWidth() > 760*ui::textScale()) {
        ImGui::SameLine(0,10*s); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Fast Enough");
    }
    ImGui::SameLine(0,24*s);
    auto tab = [&](const char* name, bool selected) {
        if (selected) ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if(ui::semiboldFont)ImGui::PushFont(ui::semiboldFont);
        bool pressed = ui::button(name,ui::Icon::None,ui::Appearance::Quiet);
        if(ui::semiboldFont)ImGui::PopFont();
        if (selected) ImGui::PopStyleColor();
        if (selected) {
            ImVec2 a=ImGui::GetItemRectMin(), b=ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x+8*s,b.y+6*s),ImVec2(b.x-8*s,b.y+6*s),ImGui::GetColorU32(ImGuiCol_CheckMark),2*s);
        }
        return pressed;
    };
    if (tab("Files",!m_showAppsWorkspace&&!m_showBackupManager&&!m_showConnectionsWorkspace)) {
        m_showAppsWorkspace=m_showBackupManager=m_showConnectionsWorkspace=false;
        m_lastFocusedPanel=&m_rightPanel;
    }
    ImGui::SameLine(0,4*s);
    if (tab("Apps",m_showAppsWorkspace)) {
        if (!m_showAppsWorkspace) {
            int slot=m_lastFocusedPanel?m_lastFocusedPanel->deviceSlot:0;
            m_appsWorkspacePanel.isAndroid=true;
            m_appsWorkspacePanel.isApps=true;
            m_appsWorkspacePanel.deviceSlot=slot;
            m_appsWorkspacePanel.needsRefresh=true;
        }
        m_showAppsWorkspace=true; m_showBackupManager=m_showConnectionsWorkspace=false;
    }
    ImGui::SameLine(0,4*s);
    if (tab("Backups",m_showBackupManager)) {
        if (!m_showBackupManager) {
            m_backupManagerNeedsAppRefresh=m_backupManagerNeedsBackupRefresh=true;
            m_backupRootAccess=BackupRootAccess::Unknown;
            m_backupRootAccessSerial.clear();
        }
        m_showBackupManager=true; m_showAppsWorkspace=m_showConnectionsWorkspace=false;
    }
    ImGui::SameLine(0,4*s);
    if (tab("Connections",m_showConnectionsWorkspace)) {
        m_showConnectionsWorkspace=true;
        m_showAppsWorkspace=m_showBackupManager=false;
        m_connectionsWorkspacePanel.isConnections=true;
    }
    int ready=0;
    for (int i=0;i < deviceSessionCount();++i) if (deviceSession(i).connected&&deviceSession(i).client.isServerRunning()) ++ready;
    std::string deviceLabel=std::to_string(ready)+(ready==1?" phone ready":" phones ready");
    if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(deviceLabel.c_str()).x+156*s)
        deviceLabel=std::to_string(ready);
    float rightWidth=ImGui::CalcTextSize(deviceLabel.c_str()).x+132*s;
    ImGui::SameLine(std::max(ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x+12*s,ImGui::GetWindowWidth()-rightWidth));
    if (ui::button((deviceLabel+"##Devices").c_str(),ui::Icon::Phone)) ImGui::OpenPopup("##WorkspaceDevices");
    if (ImGui::BeginPopup("##WorkspaceDevices")) {
        for (int slot=0;slot < deviceSessionCount();++slot) {
            if (deviceSession(slot).serial.empty()) continue;
            std::string name=deviceDisplayName(slot)+(deviceSession(slot).connected?"":" (disconnected)")+"##device"+std::to_string(slot);
            if (ImGui::MenuItem(name.c_str())) { m_detailsDeviceSlot=slot; m_workspaceDrawer=2; }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Connect a device...")) {
            m_showConnectionsWorkspace=true; m_showAppsWorkspace=m_showBackupManager=false;
            m_connectionsWorkspacePanel.isConnections=true;
        }
        if (ImGui::MenuItem("Pair over WiFi...")) openWifiPairing();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0,2*s);
    if (ui::button("##WorkspaceSettings",ui::Icon::Settings,ui::Appearance::Quiet,ImVec2(32*s,32*s),"Workspace settings")) m_workspaceDrawer=3;
    ImGui::SameLine(0,2*s);
    if (ui::button("##WorkspaceMore",ui::Icon::More,ui::Appearance::Quiet,ImVec2(32*s,32*s),"All commands")) ImGui::OpenPopup("##WorkspaceMenu");
    renderMenuBar();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void App::renderWorkspaceToolbar() {
    const float s=ui::scale();
    FilePanel& source=m_lastFocusedPanel==&m_leftPanel?m_leftPanel:m_rightPanel;
    FilePanel& destination=&source==&m_leftPanel?m_rightPanel:m_leftPanel;
    bool fileView=!source.isApps&&!source.isConnections;
    bool hasSelection=fileView&&!source.selectedIndices.empty();
    bool canTransfer=hasSelection&&panelAvailable(source)&&panelAvailable(destination)&&!destination.isApps;
    if (source.isAndroid&&destination.isAndroid&&source.deviceSlot==destination.deviceSlot) canTransfer=false;
    bool canMove=canTransfer&&!source.isAndroid&&!destination.isAndroid;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(16*s,6*s));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,0);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("##WorkspaceToolbar",ImVec2(0,ui::controlHeight()+12*s),ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_AlwaysUseWindowPadding);
    ImGui::AlignTextToFramePadding();
    std::string selection=hasSelection?std::to_string(source.selectedIndices.size())+" selected, "+formatSize(source.selectedFileSize()):"Select files to copy";
    bool wide=ImGui::GetWindowWidth()>980*ui::textScale();
    ui::textClipped(selection,(wide?150.0f:110.0f)*s,ImGui::GetColorU32(ImGuiCol_TextDisabled),ui::controlHeight());
    ImGui::SameLine(0,10*s);
    ImGui::BeginDisabled(!canTransfer);
    std::string name=panelDisplayName(destination);
    std::string copyLabel="Copy to "+name;
    float copyWidth=std::min(230*s,ImGui::CalcTextSize(copyLabel.c_str()).x+45*s);
    if(ui::button(copyLabel.c_str(),ui::Icon::Copy,ui::Appearance::Primary,ImVec2(copyWidth,32*s),copyLabel.c_str())) startWorkspaceTransfer(false);
    ImGui::EndDisabled();
    if(wide) {
        ImGui::SameLine(0,4*s); ImGui::BeginDisabled(!canMove);
        if(ui::button("Move",ui::Icon::Move,ui::Appearance::Secondary)) startWorkspaceTransfer(true);
        ImGui::EndDisabled();
        ui::tooltip("Move files between local folders. Use Copy for transfers involving a phone.");
        ImGui::SameLine(0,4*s); ImGui::BeginDisabled(!hasSelection||!panelAvailable(source));
        if(ui::button("##ToolbarDelete",ui::Icon::Trash,ui::Appearance::Quiet,ImVec2(32*s,32*s),"Delete selected files")) {
            m_contextPanel=&source; m_contextIndex=*source.selectedIndices.begin(); m_deletePermanent=false; m_showDeleteConfirm=true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0,12*s); ImGui::BeginDisabled(!fileView||!panelAvailable(source));
        if(ui::button("New folder",ui::Icon::Plus,ui::Appearance::Secondary)) { m_contextPanel=&source; m_newFolderName[0]=0; m_showNewFolderPopup=true; }
        ImGui::EndDisabled();
    }
    ImGui::SameLine(0,4*s);
    if(ui::button("##FileActions",ui::Icon::More,ui::Appearance::Quiet,ImVec2(32*s,32*s),"More file actions")) ImGui::OpenPopup("##FileActionsMenu");
    if(ImGui::BeginPopup("##FileActionsMenu")) {
        if(ImGui::MenuItem("Move to other pane",nullptr,false,canMove)) startWorkspaceTransfer(true);
        if(ImGui::MenuItem("Rename...","F2",false,hasSelection&&source.selectedIndices.size()==1&&panelAvailable(source))) {
            m_contextPanel=&source; m_contextIndex=*source.selectedIndices.begin(); strcpy_s(m_renameBuf,source.entryName(m_contextIndex).c_str());m_showRenamePopup=true;
        }
        if(ImGui::MenuItem("Delete...","Del",false,hasSelection&&panelAvailable(source))) {
            m_contextPanel=&source;m_contextIndex=*source.selectedIndices.begin();m_deletePermanent=false;m_showDeleteConfirm=true;
        }
        if(ImGui::MenuItem("New folder...",nullptr,false,fileView&&panelAvailable(source))) {
            m_contextPanel=&source;m_newFolderName[0]=0;m_showNewFolderPopup=true;
        }
        ImGui::Separator();
        if(ImGui::MenuItem("Show hidden files",nullptr,source.showHidden)) { source.showHidden=!source.showHidden;source.needsRefresh=true; }
        ImGui::EndPopup();
    }
    float trailingWidth=ImGui::CalcTextSize("Compare").x+97*s;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(),ImGui::GetWindowWidth()-trailingWidth));
    ImGui::BeginDisabled(!fileView||destination.isApps||destination.isConnections);
    if(ui::button("Compare",ui::Icon::Compare,m_compareEnabled?ui::Appearance::Selected:ui::Appearance::Quiet)) {
        m_compareEnabled=!m_compareEnabled;m_compareDirty=true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0,4*s);ImGui::BeginDisabled(!hasSelection);
    if(ui::button("##SelectionDetails",ui::Icon::Info,ui::Appearance::Quiet,ImVec2(32*s,32*s),"Selection details")) m_workspaceDrawer=1;
    ImGui::EndDisabled();
    ImGui::EndChild();ImGui::PopStyleColor();ImGui::PopStyleVar(2);
}

void App::renderPaneHeader(FilePanel& panel, PanelSide side) {
    const float s=ui::scale();
    ImGui::PushID(&panel);
    std::string title=panelDisplayName(panel);
    std::string caption=panel.isConnections?"USB and wireless connections":"Local storage";
    if(panel.isAndroid||panel.isApps) {
        int slot=panel.deviceSlot;
        caption=panelAvailable(panel)?"Connected":"Disconnected";
        if (!panel.pendingDeviceSerial.empty()) caption=panel.deviceOpenError.empty()?"Opening files...":"Connection unavailable";
        if(panelAvailable(panel)) {
            caption+=deviceSession(slot).client.isDirectConnection()?"  /  Direct connection":"  /  ADB";
            if(slot==0&&m_dualChannelAvailable)caption+=" + "+m_secondaryChannelType;
        }
    }
    ImVec2 origin=ImGui::GetCursorScreenPos();
    float width=ImGui::GetContentRegionAvail().x;
    bool showRoot = panel.isAndroid && !panel.isApps && !panel.isConnections && panel.pendingDeviceSerial.empty();
    float rootWidth = showRoot ? ImGui::CalcTextSize(deviceSession(panel.deviceSlot).rootRequestPending ? "ROOT..." :
        deviceSession(panel.deviceSlot).client.useRoot() ? "ROOT ON" : "ROOT").x + 28*s : 0;
    float headerHeight = std::max(44*s, ImGui::GetFontSize() + 12*ui::textScale() + 14*s);
    if (showRoot && deviceSession(panel.deviceSlot).rootRequestPending) caption = "Changing root access...";
    float locationWidth = std::max(40*s, width - 40*s - rootWidth);
    if(ui::button("##PaneLocation",ui::Icon::None,ui::Appearance::Quiet,ImVec2(locationWidth,headerHeight),"Choose a location"))
        ImGui::OpenPopup("##PaneLocationMenu");
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(origin.x,origin.y+5*s),ImVec2(origin.x+32*s,origin.y+39*s),ImGui::GetColorU32(ImGuiCol_Button),6*s);
    ui::icon(panel.isAndroid||panel.isApps||panel.isConnections?ui::Icon::Phone:ui::Icon::Computer,
        ImVec2(origin.x+6*s,origin.y+11*s),20*s,ImGui::GetColorU32(ImGuiCol_TextDisabled));
    float textWidth=std::max(1.0f,locationWidth-62*s);
    if(ui::semiboldFont)ImGui::PushFont(ui::semiboldFont);
    ui::drawTextClipped(ImVec2(origin.x+44*s,origin.y+4*s),title,textWidth,ImGui::GetColorU32(ImGuiCol_Text));
    float arrowX=origin.x+44*s+std::min(textWidth,ImGui::CalcTextSize(title.c_str()).x)+6*s;
    if(ui::semiboldFont)ImGui::PopFont();
    ui::icon(ui::Icon::Down,ImVec2(arrowX,origin.y+4*s),14*s,ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ui::drawTextClipped(ImVec2(origin.x+44*s,origin.y+ImGui::GetFontSize()+11*s),caption,
        std::max(1.0f,locationWidth-48*s),ImGui::GetColorU32(ImGuiCol_TextDisabled),12*ui::textScale());
    if (showRoot) {
        ImGui::SameLine(0,8*s);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + locationWidth + 8*s, origin.y + (headerHeight-ui::controlHeight())*0.5f));
        renderDeviceRootButton(panel.deviceSlot, &panel);
    }
    ImGui::SameLine(0,8*s);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()+6*s);
    if(ui::button("##PaneInfo",ui::Icon::Info,ui::Appearance::Quiet,ImVec2(32*s,32*s),panel.isAndroid||panel.isApps?"Device details":"Local file details")) {
        if(panel.isAndroid||panel.isApps){m_detailsDeviceSlot=panel.deviceSlot;m_workspaceDrawer=2;}
        else{m_lastFocusedPanel=&panel;m_workspaceDrawer=1;}
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(280*s,0),ImVec2(460*s,500*s));
    if(ImGui::BeginPopup("##PaneLocationMenu")) {
        if(!panel.isApps&&ImGui::Selectable("This PC",!panel.isAndroid&&!panel.isConnections)) switchPanelMode(panel,false);
        for(int slot=0;slot < deviceSessionCount();++slot) {
            if(deviceSession(slot).serial.empty()&&!deviceSession(slot).connected) continue;
            std::string serial=deviceSession(slot).serial;
            std::string suffix=serial.size()>6?serial.substr(serial.size()-6):serial;
            std::string label=deviceDisplayName(slot)+" ("+suffix+")"+(!deviceSession(slot).connected?(deviceForSlot(slot).isConnecting()?" [preparing]":" [offline]"):"")+"##"+std::to_string(slot);
            if(ImGui::Selectable(label.c_str(),(panel.isAndroid||panel.isApps)&&panel.deviceSlot==slot)) {
                panel.pendingDeviceSerial.clear();panel.pendingDeviceName.clear();panel.deviceOpenError.clear();
                if(panel.isApps) {
                    panel.deviceSlot=slot;panel.appEntries.clear();panel.selectedIndices.clear();panel.needsRefresh=true;
                } else {
                    openDeviceSession(panel, slot);
                }
            }
        }
        ImGui::Separator();
        if(ImGui::Selectable("Connect a device...")) {
            m_showConnectionsWorkspace=true;m_showAppsWorkspace=m_showBackupManager=false;m_connectionsWorkspacePanel.isConnections=true;
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();
    ImGui::PopID();
}

void App::renderPaneNavigation(FilePanel& panel) {
    const float s=ui::scale();
    const float height=std::max(ui::controlHeight(28), ImGui::GetFrameHeight());
    bool requestEdit=m_lastFocusedPanel==&panel && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L);
    if(requestEdit){panel.editingPath=true;panel.pathFocusRequested=true;strcpy_s(panel.pathInput,panel.currentPath.c_str());}
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
    ImGui::BeginChild("##PaneNavigation",ImVec2(0,height),ImGuiChildFlags_None,ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    auto history=[&](int step) {
        panel.navHistoryPos += step;
        const std::string path = panel.navHistory[panel.navHistoryPos];
        navigateToDirectory(panel, path);
        strcpy_s(panel.pathInput, panel.currentPath.c_str());
    };
    ImGui::BeginDisabled(panel.navHistoryPos<=0);
    if(ui::button("##Back",ui::Icon::Back,ui::Appearance::Quiet,ImVec2(26*s,28*s),"Back"))history(-1);
    ImGui::EndDisabled();ImGui::SameLine(0,0);
    ImGui::BeginDisabled(panel.navHistoryPos<0||panel.navHistoryPos+1>=(int)panel.navHistory.size());
    if(ui::button("##Forward",ui::Icon::Forward,ui::Appearance::Quiet,ImVec2(26*s,28*s),"Forward"))history(1);
    ImGui::EndDisabled();ImGui::SameLine(0,0);
    if(ui::button("##Up",ui::Icon::Up,ui::Appearance::Quiet,ImVec2(26*s,28*s),"Up one folder"))navigateUp(panel);
    ImGui::SameLine(0,5*s);
    float available=ImGui::GetContentRegionAvail().x;
    if(panel.editingPath) {
        ImGui::SetNextItemWidth(available);
        if(requestEdit || panel.pathFocusRequested){ImGui::SetKeyboardFocusHere();panel.pathFocusRequested=false;}
        if(ImGui::InputText("##FolderPath",panel.pathInput,sizeof(panel.pathInput),ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_AutoSelectAll)) {
            std::string path=panel.pathInput;
            if(!path.empty()){navigateToDirectory(panel,path);panel.editingPath=false;}
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Escape))panel.editingPath=false;
    } else {
        std::vector<std::pair<std::string,std::string>> crumbs;
        std::string path=panel.currentPath;
        char separator=panel.isAndroid?'/':'\\';
        size_t offset=0;
        if(panel.isAndroid&&path.starts_with('/')){crumbs.push_back({"/","/"});offset=1;}
        else if(path.size()>=3&&path[1]==':'){crumbs.push_back({path.substr(0,2),path.substr(0,3)});offset=3;}
        while(offset<path.size()){
            size_t next=path.find(separator,offset);if(next==std::string::npos)next=path.size();
            if(next>offset)crumbs.push_back({path.substr(offset,next-offset),path.substr(0,next)});
            offset=next+1;
        }
        float total=0;for(auto& crumb:crumbs)total+=ImGui::CalcTextSize(crumb.first.c_str()).x+34*s;
        bool condensed=total>available-40*s;
        if(condensed) {
            if(ui::button("##ParentFolders",ui::Icon::More,ui::Appearance::Quiet,ImVec2(28*s,28*s),"Parent folders"))ImGui::OpenPopup("##ParentFolderMenu");
            if(ImGui::BeginPopup("##ParentFolderMenu")) {
                for(auto& crumb:crumbs)if(ImGui::MenuItem(crumb.second.c_str()))navigateToDirectory(panel,crumb.second);
                ImGui::EndPopup();
            }
            ImGui::SameLine(0,2*s);
        }
        size_t first=condensed&&!crumbs.empty()?crumbs.size()-1:0;
        for(size_t i=first;i<crumbs.size();++i){
            if(i>first){ImGui::SameLine(0,0);ImGui::AlignTextToFramePadding();ImGui::TextDisabled(">");ImGui::SameLine(0,0);}
            ImGui::PushID((int)i);
            float remaining=std::max(24*s,ImGui::GetContentRegionAvail().x-36*s);
            float width=std::min(remaining,ImGui::CalcTextSize(crumbs[i].first.c_str()).x+20*s);
            if(ui::button(crumbs[i].first.c_str(),ui::Icon::None,ui::Appearance::Quiet,ImVec2(width,28*s),crumbs[i].second.c_str()))navigateToDirectory(panel,crumbs[i].second);
            ImGui::PopID();
        }
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(),ImGui::GetWindowWidth()-32*s));
        if(ui::button("##EditPath",ui::Icon::File,ui::Appearance::Quiet,ImVec2(28*s,28*s),"Edit folder path (Ctrl+L)")){panel.editingPath=true;panel.pathFocusRequested=true;strcpy_s(panel.pathInput,panel.currentPath.c_str());}
    }
    ImGui::EndChild();
}

float App::transferDockHeight() {
    const float s = ui::scale();
    std::lock_guard<std::mutex> lock(m_batchMutex);
    if (m_batchQueue.empty()) return 0;
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x-28*s);
    const auto batch = visibleTransfer(m_batchQueue);
    ui::TransferCardModel card;
    card.canCancel = !finished(batch->state.load()) && !batch->stopRequested;
    return ui::transferCardHeight(card,width)+16*s+
        (m_showTransferChannels ? std::max(100*s,ImGui::GetTextLineHeightWithSpacing()*4)+12*s : 0);
}

void App::renderTransferDock(float height) {
    if (height <= 0) return;
    const float s = ui::scale();
    std::vector<std::shared_ptr<TransferBatch>> batches;
    { std::lock_guard<std::mutex> lock(m_batchMutex); batches.assign(m_batchQueue.begin(),m_batchQueue.end()); }
    if (batches.empty()) return;
    auto batch = visibleTransfer(batches);
    int queued = 0;
    for (auto& candidate : batches) {
        const auto state = candidate->state.load();
        if (state == BatchState::Queued) ++queued;
    }

    const auto state = batch->state.load();
    const bool done = finished(state);
    const bool preparing = state == BatchState::Preparing;
    const bool waiting = batch->waitingConflict && !done;
    const bool disconnected = batch->disconnected && !done;
    const bool cancelling = batch->stopRequested && !done;
    const bool paused = !done && (state == BatchState::Paused || batch->pauseRequested);
    const bool running = state == BatchState::Running && !disconnected && !paused && !cancelling;
    const bool completed = state == BatchState::Completed;
    const bool errors = state == BatchState::Failed || (completed && (batch->errorSkippedFiles > 0 || batch->crcFailCount() > 0));
    const int count = preparing ? 0 : batch->totalFiles();
    const uint64_t transferred = batch->totalTransferred.load();
    const uint64_t total = batch->totalBytes.load();
    const std::string source = batch->isLocalCopy || (!batch->isPull && !batch->isCrossDevice) ? "This PC" : deviceDisplayName(batch->srcDeviceSlot);
    const std::string destination = batch->isLocalCopy || (batch->isPull && !batch->isCrossDevice) ? "This PC" : deviceDisplayName(batch->dstDeviceSlot);

    ui::TransferCardModel card;
    card.heading = cancelling ? "Cancelling transfer" : waiting ? "Your decision is needed" : disconnected ? "Connection interrupted" :
        errors ? (completed ? "Finished with errors" : "Transfer failed") : completed ? "Transfer complete" :
        paused ? "Transfer paused" : state == BatchState::Stopped ? "Transfer cancelled" :
        preparing ? "Preparing transfer" : state == BatchState::Queued ? "Transfer queued" :
        state == BatchState::Verifying ? "Checking your files" : batch->isMove ? "Moving your files" : "Copying your files";
    card.tone = cancelling || state == BatchState::Stopped ? ui::TransferTone::Neutral :
        waiting || disconnected || paused ? ui::TransferTone::Attention : errors ? ui::TransferTone::Error :
        completed ? ui::TransferTone::Success : ui::TransferTone::Active;
    card.route = source+"  >  "+destination;
    card.filename = preparing ? "Finding files and folders" : count == 1 ? batch->files.front().displayName : std::to_string(count)+" items";
    card.amount = preparing ? std::to_string(batch->discoveredItems.load())+" items found" :
        formatSize(transferred)+" of "+formatSize(total);
    card.progress = completed ? 1.0f : state == BatchState::Verifying ? batch->crcProgress.load() : batch->totalProgress.load();
    card.indeterminate = preparing || state == BatchState::Queued || cancelling;
    card.paused = paused;
    card.canCancel = !done && !cancelling;
    card.canReview = waiting && !cancelling;
    card.canRetry = disconnected && !waiting && !cancelling;
    card.canPause = !waiting && !disconnected && !cancelling && (state == BatchState::Running || state == BatchState::Paused);
    card.showChannels = m_showTransferChannels;
    card.queued = std::max(0,queued-(state == BatchState::Queued ? 1 : 0));
    if (waiting) {
        card.detail = "A file already exists at the destination. Choose Replace or Skip to continue.";
        card.metric = "Waiting for you";
    } else if (disconnected) {
        card.detail = "Reconnect your device to continue, or choose Retry now.";
        card.metric = "Waiting for device";
    } else if (errors) {
        card.detail = "Some files could not be transferred or checked. Open Details to review.";
        card.metric = "Review errors";
        card.eta = "Saved in Activity log";
    } else if (completed) {
        card.detail = batch->skippedFiles > 0 ? std::to_string(batch->skippedFiles)+" skipped, as requested" :
            !batch->crcResults.empty() ? "Files checked successfully" : "All items transferred";
        card.metric = formatSpeed(batch->finalAvgSpeed.load())+" average";
        card.eta = "Finished in "+formatETA(batch->finalTimeSec.load());
    } else if (state == BatchState::Stopped) {
        card.detail = "Transfer stopped. Details are saved in Activity log.";
    } else if (cancelling) {
        card.detail = "Stopping active connections...";
    } else if (preparing) {
        card.detail = "You can keep browsing while your files are prepared.";
    } else if (state == BatchState::Queued) {
        card.detail = "Starts automatically when the current transfer finishes.";
    } else if (paused) {
        card.detail = "Choose Resume when you are ready to continue.";
    } else if (state == BatchState::Verifying) {
        card.detail = "Checking that the copied files match the originals.";
        card.metric = "Verifying";
    } else {
        card.detail = count > 1 ? std::to_string(std::min(batch->completedFiles(),count))+" of "+std::to_string(count)+" items finished" :
            count == 1 ? "To "+parentPath(batch->files.front().destPath) : "Preparing files...";
        if (running) {
            const double speed = batch->speedBytesPerSec.load();
            card.metric = speed > 0 ? formatSpeed(speed) : "Starting...";
            const double eta = batch->etaSeconds.load();
            if (eta >= 0) card.eta = formatETA(eta)+" remaining";
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(14*s,8*s));
    ImGui::BeginChild("##TransferDock",ImVec2(0,height),ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto action = ui::transferCard(card,ImGui::GetContentRegionAvail().x);
    if (action == ui::TransferCardAction::Details) {
        m_transferDetailsBatch = batch;
        m_overlayVisible = true;
    } else if (action == ui::TransferCardAction::Channels) {
        m_showTransferChannels = !m_showTransferChannels;
    } else if (action == ui::TransferCardAction::PauseResume) {
        batch->logEvent(LogLevel::Info,paused ? "Resume requested" : "Pause requested");
        {
            std::lock_guard<std::mutex> lock(m_batchMutex);
            batch->pauseRequested = !paused;
        }
        m_batchCV.notify_all();
    } else if (action == ui::TransferCardAction::Retry) {
        batch->logEvent(LogLevel::Info,"Retry requested");
        {
            std::lock_guard<std::mutex> lock(m_batchMutex);
            ++batch->retryGeneration;
            batch->userRetryRequested = true;
        }
        m_batchCV.notify_all();
    } else if (action == ui::TransferCardAction::Review) {
        m_transferAttentionFocus = true;
    } else if (action == ui::TransferCardAction::Cancel) {
        m_cancelDockBatch = batch;
        ImGui::OpenPopup("##CancelDockTransfer");
    }
    if (ImGui::BeginPopup("##CancelDockTransfer")) {
        auto pendingCancel = m_cancelDockBatch.lock();
        if (!pendingCancel || finished(pendingCancel->state.load())) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextUnformatted("Cancel this transfer?");
            ImGui::Spacing();
            if (ui::button("Keep transferring",ui::Icon::Play,ui::Appearance::Primary)) ImGui::CloseCurrentPopup();
            if (ui::button("Cancel transfer",ui::Icon::Close,ui::Appearance::Secondary)) {
                pendingCancel->logEvent(LogLevel::Info,"Cancel requested");
                {
                    std::lock_guard<std::mutex> lock(m_batchMutex);
                    pendingCancel->requestStop();
                    pendingCancel->userRetryRequested = false;
                }
                m_batchCV.notify_all();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    if (m_showTransferChannels) {
        ImGui::Spacing();
        ImGui::BeginChild("##DockDetails",ImVec2(0,0),ImGuiChildFlags_None);
        bool hasChannels = false;
        for (int i = 0; i < TransferBatch::MAX_CHANNELS; ++i) {
            const auto& channel = batch->channels[i];
            const auto bytes = channel.bytesTransferred.load();
            const auto assigned = channel.totalBytes.load();
            const double speed = running ? channel.speed.load() : 0;
            if (bytes == 0 && assigned == 0 && speed == 0) continue;
            hasChannels = true;
            ImGui::Text("Channel %d    %s    %s transferred",i+1,formatSpeed(speed).c_str(),formatSize(bytes).c_str());
        }
        if (!hasChannels) ImGui::TextDisabled("%s",done ? "Transfer finished" : "Connection speeds appear once transferring starts.");
        for (auto& queuedBatch : batches) {
            if (queuedBatch == batch || finished(queuedBatch->state.load())) continue;
            ImGui::PushID(queuedBatch.get());
            std::string label = std::string(batchStateLabel(*queuedBatch));
            if (queuedBatch->state != BatchState::Preparing) label += ": "+std::to_string(queuedBatch->totalFiles())+" items";
            if (ImGui::Selectable(label.c_str())) { m_transferDetailsBatch = queuedBatch; m_overlayVisible = true; }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void App::renderWorkspaceDrawer() {
    if(!m_workspaceDrawer)return;
    const float s=ui::scale();
    auto* viewport=ImGui::GetMainViewport();
    float width=std::min(400*s,viewport->WorkSize.x-24*s);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x+viewport->WorkSize.x-width-8*s,viewport->WorkPos.y+58*s));
    ImGui::SetNextWindowSize(ImVec2(width,std::max(200*s,viewport->WorkSize.y-96*s)));
    const char* title=m_workspaceDrawer==1?"File details":m_workspaceDrawer==2?"Device details":"Workspace settings";
    bool open=true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20*s,18*s));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
    if(ImGui::Begin("##WorkspaceDrawer",&open,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoTitleBar)) {
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(title);
        ImGui::SameLine(ImGui::GetWindowWidth()-54*s);
        if(ui::button("##CloseDrawer",ui::Icon::Close,ui::Appearance::Quiet,ImVec2(30*s,30*s),"Close details"))m_workspaceDrawer=0;
        ImGui::Separator();ImGui::Spacing();
        if(m_workspaceDrawer==3) {
            const char* modes[]={"Follow Windows","Dark","Light"};
            ImGui::TextUnformatted("Appearance");ImGui::SetNextItemWidth(-1);
            if(ImGui::Combo("##WorkspaceTheme",&m_theme.mode,modes,3)){m_theme.customColors=false;m_pendingScale=m_theme.userScale;m_theme.save();}
            ImGui::Spacing();ImGui::TextUnformatted("File rows");
            int density=m_prefs.compactRows?1:0;const char* densities[]={"Comfortable","Compact"};ImGui::SetNextItemWidth(-1);
            if(ImGui::Combo("##WorkspaceDensity",&density,densities,2)){m_prefs.compactRows=density==1;m_prefs.save();}
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
            if(ui::button("Customize appearance",ui::Icon::Settings,ui::Appearance::Secondary))m_showThemeWindow=true;
            if(ui::button("Preferences",ui::Icon::Settings,ui::Appearance::Secondary))m_showPreferences=true;
            if(ui::button("Activity log",ui::Icon::File,ui::Appearance::Secondary)){m_showDebugWindow=true;m_workspaceDrawer=0;}
            ImGui::Spacing();ImGui::TextDisabled("Keyboard shortcuts");
            ImGui::TextUnformatted("Ctrl+A    Select all\nF2           Rename\nF5           Refresh folders\nDelete    Delete selection\nCtrl+L    Edit folder path\nEscape  Close details or clear selection");
        } else if(m_workspaceDrawer==2) {
            int slot=m_detailsDeviceSlot;
            ImGui::SetNextItemWidth(-1);
            if(ImGui::BeginCombo("##DetailsDevice",deviceDisplayName(slot).c_str())) {
                for(int i=0;i < deviceSessionCount();++i)if(!deviceSession(i).serial.empty()) {
                    if(ImGui::Selectable((deviceDisplayName(i)+"##"+std::to_string(i)).c_str(),i==slot))m_detailsDeviceSlot=i;
                }
                ImGui::EndCombo();
            }
            bool connected=deviceSession(slot).connected&&deviceSession(slot).client.isServerRunning();
            ImGui::TextDisabled("%s",connected?"Connected":"Disconnected");
            ImGui::Spacing();
            renderDeviceRootButton(slot);
            ImGui::Spacing();ImGui::TextDisabled("Connection");
            ImGui::TextWrapped("%s",deviceSession(slot).client.isDirectConnection()?"Direct connection":"ADB connection");
            if(slot==0&&m_dualChannelAvailable)ImGui::TextDisabled("Additional connection: %s",m_secondaryChannelType.c_str());
            auto& prefs=m_prefs.pipePreferencesFor(deviceSession(slot).serial);
            ImGui::Spacing();
            if(ImGui::CollapsingHeader("Advanced connection settings")) {
                ImGui::TextWrapped("Requested streams apply when transfer channels are opened.");
                ImGui::TextUnformatted("USB streams");
                ImGui::SetNextItemWidth(-1);if(ImGui::SliderInt("##USBStreams",&prefs.usbPipeCount,1,4)){m_prefs.save();}
                ImGui::TextUnformatted("WiFi streams");
                ImGui::SetNextItemWidth(-1);if(ImGui::SliderInt("##WiFiStreams",&prefs.wifiPipeCount,1,4)){m_prefs.save();}
                if(ui::button("Network adapters",ui::Icon::Wifi))m_showNicConfig=true;
                if(ui::button("Root, keep awake and ADB",ui::Icon::Settings))m_showDeviceSettings=true;
            }
            ImGui::Spacing();ImGui::Separator();ImGui::Spacing();ImGui::TextDisabled("Windows integration");
            auto& mount=DeviceMountManager::instance(slot);
            ImGui::BeginDisabled(!connected||m_asyncBusy||deviceSession(slot).rootRequestPending);
            if(mount.isMounted()) {
                if(ui::button("Open mounted drive",ui::Icon::Computer,ui::Appearance::Secondary)) {
                    auto path=workspaceWide(mount.mountPoint());ShellExecuteW(nullptr,L"explore",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
                }
                if(ui::button("Unmount drive"))postAsync("Unmounting drive...",[slot](){DeviceMountManager::instance(slot).unmount();});
            } else if(ui::button("Mount as drive",ui::Icon::Computer,ui::Appearance::Secondary)) {
                std::string root=deviceSession(slot).storageRoot,drive=DeviceMountManager::availableMountPoint();
                postAsync("Mounting drive...",[this,slot,root,drive](){
                    bool ok=DeviceMountManager::instance(slot).mount(&deviceSession(slot).client,root,drive);
                    m_statusMessage=ok?"Mounted as "+drive:"Unable to mount the drive. Check the Dokan installation.";
                    m_statusTime=std::chrono::steady_clock::now();
                });
            }
            ImGui::EndDisabled();
            ImGui::Spacing();if(ui::button("Pair over WiFi",ui::Icon::Wifi))openWifiPairing();
            ImGui::BeginDisabled(!connected);
            if(ui::button("WiFi setup wizard",ui::Icon::Wifi)){if(!m_wizardBusy)m_wizardSerial=deviceSession(slot).serial;openWifiWizard();}
            ImGui::EndDisabled();
        } else if(m_workspaceDrawer==1) {
            FilePanel* panel=m_lastFocusedPanel?m_lastFocusedPanel:&m_rightPanel;
            ImGui::TextWrapped("%s",panelDisplayName(*panel).c_str());
            ImGui::TextDisabled("Location");ImGui::TextWrapped("%s",panel->currentPath.c_str());ImGui::Spacing();
            if(panel->selectedIndices.empty())ImGui::TextDisabled("Select a file to see its details.");
            else {
                ImGui::Text("%d selected",(int)panel->selectedIndices.size());
                ImGui::TextDisabled("Total file size: %s",formatSize(panel->selectedFileSize()).c_str());
                if(panel->selectedIndices.size()==1) {
                    int index=*panel->selectedIndices.begin();
                    if(panel->validIndex(index)) {
                        std::string name=panel->entryName(index);
                        ImGui::Spacing();ImGui::TextWrapped("%s",name.c_str());
                        ImGui::TextDisabled("%s",panel->entryDate(index).c_str());
                        bool mcraw=name.size()>6&&_stricmp(name.c_str()+name.size()-6,".mcraw")==0;
                        ImGui::BeginDisabled(!panelAvailable(*panel));
                        if(mcraw&&ui::button("Browse frames",ui::Icon::Folder,ui::Appearance::Secondary)) {
                            std::string path=panel->currentPath;char sep=panel->isAndroid?'/':'\\';if(!path.empty()&&path.back()!=sep)path+=sep;path+=name;
                            if(panel->isAndroid)navigateToDirectory(*panel,path);
                            else requestMcrawMount(*panel,path);
                            m_workspaceDrawer=0;
                        }
                        if(ui::button("Rename",ui::Icon::File)){m_contextPanel=panel;m_contextIndex=index;strcpy_s(m_renameBuf,name.c_str());m_showRenamePopup=true;}
                        ImGui::EndDisabled();
                    }
                }
            }
        }
        if(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)&&ImGui::IsKeyPressed(ImGuiKey_Escape))m_workspaceDrawer=0;
    }
    ImGui::End();ImGui::PopStyleColor();ImGui::PopStyleVar();
    if(!open)m_workspaceDrawer=0;
}
