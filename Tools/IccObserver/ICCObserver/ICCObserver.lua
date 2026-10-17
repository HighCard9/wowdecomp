local f = CreateFrame("Frame", "ICCObserverPanel", UIParent)
f:SetSize(440, 110)
f:SetPoint("TOP", UIParent, "TOP", 0, -100)
f:SetBackdrop({bgFile="Interface\\Tooltips\\UI-Tooltip-Background", edgeFile="Interface\\Tooltips\\UI-Tooltip-Border", tile=true, tileSize=16, edgeSize=16, insets={left=4,right=4,top=4,bottom=4}})
f:SetBackdropColor(0, 0, 0, 0.85)
f:SetMovable(true)
f:EnableMouse(true)
f:RegisterForDrag("LeftButton")
f:SetScript("OnDragStart", f.StartMoving)
f:SetScript("OnDragStop", f.StopMovingOrSizing)
local title = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
title:SetPoint("TOPLEFT", 12, -10)
title:SetText("ICC route preview | blue: estimated trails | green: proposal")
local text = f:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
text:SetPoint("TOPLEFT", 12, -32)
text:SetWidth(416)
text:SetJustifyH("LEFT")
local elapsed = 0
f:SetScript("OnUpdate", function(self, dt)
    elapsed = elapsed + dt
    if elapsed < 0.25 then return end
    elapsed = 0
    text:SetText(GetCVar("iccObserverStatus") or "This panel requires WowICCObserver64.")
end)
SLASH_ICCOBSERVER1 = "/iccpreview"
SlashCmdList.ICCOBSERVER = function(message)
    if not GetCVar("iccObserver") then
        DEFAULT_CHAT_FRAME:AddMessage("Launch WowICCObserver64 to use the preview.")
        return
    end
    if message == "off" then SetCVar("iccObserver", "0"); f:Hide()
    else SetCVar("iccObserver", "1"); f:Show() end
end
f:Hide()
