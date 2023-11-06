/***********************************************************************************************************************
*                                                                                                                      *
* glscopeclient                                                                                                        *
*                                                                                                                      *
* Copyright (c) 2012-2023 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief Implementation of MainWindow
 */
#include "ngscopeclient.h"
#include "ngscopeclient-version.h"
#include "MainWindow.h"
#include "PreferenceTypes.h"
#include "FileSystem.h"

#include <iostream>
#include <fstream>

#include "DemoOscilloscope.h"
#include "RemoteBridgeOscilloscope.h"

//Dock builder API is not yet public, so might change...
#include "imgui_internal.h"

//Dialogs
#include "AddGeneratorDialog.h"
#include "AddMultimeterDialog.h"
#include "AddPowerSupplyDialog.h"
#include "AddRFGeneratorDialog.h"
#include "AddScopeDialog.h"
#include "BERTDialog.h"
#include "BERTInputChannelDialog.h"
#include "ChannelPropertiesDialog.h"
#include "FileBrowser.h"
#include "FilterPropertiesDialog.h"
#include "FunctionGeneratorDialog.h"
#include "HistoryDialog.h"
#include "LoadDialog.h"
#include "LogViewerDialog.h"
#include "ManageInstrumentsDialog.h"
#include "MeasurementsDialog.h"
#include "MetricsDialog.h"
#include "MultimeterDialog.h"
#include "PersistenceSettingsDialog.h"
#include "PowerSupplyDialog.h"
#include "PreferenceDialog.h"
#include "ProtocolAnalyzerDialog.h"
#include "RFGeneratorDialog.h"
#include "SCPIConsoleDialog.h"
#include "ScopeDeskewWizard.h"
#include "TimebasePropertiesDialog.h"
#include "TriggerPropertiesDialog.h"

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/mman.h>
#endif


using namespace std;

extern Event g_rerenderRequestedEvent;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

MainWindow::MainWindow(shared_ptr<QueueHandle> queue)
#ifdef _DEBUG
	: VulkanWindow("ngscopeclient " NGSCOPECLIENT_VERSION " [DEBUG BUILD]", queue)
#else
	: VulkanWindow("ngscopeclient " NGSCOPECLIENT_VERSION, queue)
#endif
	, m_showDemo(false)
	, m_showPlot(false)
	, m_nextWaveformGroup(1)
	, m_toolbarIconSize(0)
	, m_traceAlpha(0.75)
	, m_persistenceDecay(0.8)
	, m_session(this)
	, m_sessionClosing(false)
	, m_fileLoadInProgress(false)
	, m_openOnline(false)
	, m_showingLoadWarnings(false)
	, m_loadConfirmationChecked(false)
	, m_texmgr(queue)
	, m_needRender(false)
	, m_toneMapTime(0)
{
	LoadRecentInstrumentList();
	LoadRecentFileList();

	//Initialize command pool/buffer
	vk::CommandPoolCreateInfo poolInfo(
	vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
		queue->m_family );
	m_cmdPool = make_unique<vk::raii::CommandPool>(*g_vkComputeDevice, poolInfo);

	vk::CommandBufferAllocateInfo bufinfo(**m_cmdPool, vk::CommandBufferLevel::ePrimary, 1);
	m_cmdBuffer = make_unique<vk::raii::CommandBuffer>(
		move(vk::raii::CommandBuffers(*g_vkComputeDevice, bufinfo).front()));

	if(g_hasDebugUtils)
	{
		g_vkComputeDevice->setDebugUtilsObjectNameEXT(
			vk::DebugUtilsObjectNameInfoEXT(
				vk::ObjectType::eCommandPool,
				reinterpret_cast<int64_t>(static_cast<VkCommandPool>(**m_cmdPool)),
				"MainWindow.m_cmdPool"));

		g_vkComputeDevice->setDebugUtilsObjectNameEXT(
			vk::DebugUtilsObjectNameInfoEXT(
				vk::ObjectType::eCommandBuffer,
				reinterpret_cast<int64_t>(static_cast<VkCommandBuffer>(**m_cmdBuffer)),
				"MainWindow.m_cmdBuffer"));
	}

	UpdateFonts();

	//Load some textures
	m_toolbarIconSize = 0;
	LoadToolbarIcons();
	LoadGradients();
	m_texmgr.LoadTexture("warning", FindDataFile("icons/48x48/dialog-warning-2.png"));

	//Don't move windows when dragging in the body, only the title bar
	ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;
}

MainWindow::~MainWindow()
{
	g_vkComputeDevice->waitIdle();
	m_texmgr.clear();

	m_cmdBuffer = nullptr;
	m_cmdPool = nullptr;

	CloseSession();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Session termination

void MainWindow::CloseSession()
{
	LogTrace("Closing session\n");
	LogIndenter li;

	SaveRecentInstrumentList();

	//Close background threads in our session before destroying views
	m_session.ClearBackgroundThreads();

	//Destroy waveform views
	LogTrace("Clearing views\n");
	for(auto g : m_waveformGroups)
		g->Clear();
	m_waveformGroups.clear();
	m_newWaveformGroups.clear();
	m_splitRequests.clear();
	m_groupsToClose.clear();

	//Clear any open dialogs before destroying the session.
	//This ensures that we have a nice well defined shutdown order.
	LogTrace("Clearing dialogs\n");
	m_logViewerDialog = nullptr;
	m_metricsDialog = nullptr;
	m_timebaseDialog = nullptr;
	m_triggerDialog = nullptr;
	m_historyDialog = nullptr;
	m_preferenceDialog = nullptr;
	m_persistenceDialog = nullptr;
	m_manageInstrumentsDialog = nullptr;
	m_graphEditor = nullptr;
	m_fileBrowser = nullptr;
	m_measurementsDialog = nullptr;
	m_meterDialogs.clear();
	m_psuDialogs.clear();
	m_channelPropertiesDialogs.clear();
	m_generatorDialogs.clear();
	m_bertDialogs.clear();
	m_rfgeneratorDialogs.clear();
	m_loadDialogs.clear();
	m_dialogs.clear();
	m_protocolAnalyzerDialogs.clear();
	m_scpiConsoleDialogs.clear();

	//Clear the actual session object once all views / dialogs having handles to scopes etc have been destroyed
	m_session.Clear();

	LogTrace("Clear complete\n");

	m_sessionClosing = false;
	m_sessionFileName = "";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Add views for new instruments

string MainWindow::NameNewWaveformGroup()
{
	//TODO: avoid colliding, check if name is in use and skip if so
	int id = (m_nextWaveformGroup ++);
	return string("Waveform Group ") + to_string(id);
}

/**
	@brief Makes sure we have at least one waveform area displaying a given stream
 */
void MainWindow::AddAreaForStreamIfNotAlreadyVisible(StreamDescriptor stream)
{
	lock_guard<recursive_mutex> lock(m_waveformGroupsMutex);

	//Check if we already have a waveform area displaying it
	for(auto group : m_waveformGroups)
	{
		auto areas = group->GetWaveformAreas();
		for(auto area : areas)
		{
			if(area->IsStreamBeingDisplayed(stream))
				return;
		}
	}

	//Not here yet. Add it
	auto group = GetBestGroupForWaveform(stream);
	auto area = make_shared<WaveformArea>(stream, group, this);
	group->AddArea(area);
}

/**
	@brief Figure out what group to use for a newly added stream, based on unit compatibility etc
 */
shared_ptr<WaveformGroup> MainWindow::GetBestGroupForWaveform(StreamDescriptor /*stream*/)
{
	lock_guard<recursive_mutex> lock(m_waveformGroupsMutex);

	//If we have no waveform groups, make one
	//TODO: reject existing group if units are incompatible
	if(m_waveformGroups.empty())
	{
		//Make the group
		auto name = NameNewWaveformGroup();
		auto group = make_shared<WaveformGroup>(this, name);
		m_waveformGroups.push_back(group);

		//Group is newly created and not yet docked
		m_newWaveformGroups.push_back(group);
	}

	//Get the first compatible waveform group (may or may not be what we just created)
	//TODO: reject existing group if units are incompatible
	return *m_waveformGroups.begin();
}

/**
	@brief Handles creation of a new oscilloscope

	@param scope		The scope to add
	@param createViews	True if we should add waveform areas for each enabled channel
 */
void MainWindow::OnScopeAdded(Oscilloscope* scope, bool createViews)
{
	LogTrace("Oscilloscope \"%s\" added\n", scope->m_nickname.c_str());
	LogIndenter li;

	if(createViews)
	{
		//Add areas to it
		//For now, one area per enabled channel
		vector<StreamDescriptor> streams;

		//Headless scope? Pick every channel.
		if( (dynamic_cast<RemoteBridgeOscilloscope*>(scope)) || (dynamic_cast<DemoOscilloscope*>(scope)) )
		{
			LogTrace("Headless scope, enabling every analog channel\n");
			for(size_t i=0; i<scope->GetChannelCount(); i++)
			{
				auto chan = scope->GetOscilloscopeChannel(i);
				if(!chan)
					continue;
				for(size_t j=0; j<chan->GetStreamCount(); j++)
				{
					if(chan->GetType(j) == Stream::STREAM_TYPE_ANALOG)
						streams.push_back(StreamDescriptor(chan, j));
				}
			}

			//Handle pure logic analyzers
			if(streams.empty())
			{
				LogTrace("No analog channels found. Must be a logic analyzer. Enabling every digital channel\n");

				for(size_t i=0; i<scope->GetChannelCount(); i++)
				{
					auto chan = scope->GetOscilloscopeChannel(i);
					if(!chan)
					continue;
					for(size_t j=0; j<chan->GetStreamCount(); j++)
					{
						if(chan->GetType(j) == Stream::STREAM_TYPE_DIGITAL)
							streams.push_back(StreamDescriptor(chan, j));
					}
				}
			}
		}

		//Use whatever was enabled when we connected
		else
		{
			for(size_t i=0; i<scope->GetChannelCount(); i++)
			{
				auto chan = scope->GetOscilloscopeChannel(i);
				if(!chan)
					continue;
				if(!chan->IsEnabled())
					continue;

				for(size_t j=0; j<chan->GetStreamCount(); j++)
					streams.push_back(StreamDescriptor(chan, j));
			}
			LogTrace("%zu streams were active when we connected\n", streams.size());

			//No streams? Grab the first one.
			//TODO: can we always assume that the first channel is an oscilloscope channel?
			if(streams.empty())
			{
				LogTrace("Enabling first channel\n");
				streams.push_back(StreamDescriptor(scope->GetOscilloscopeChannel(0), 0));
			}
		}

		//Add waveform areas for the streams
		for(auto s : streams)
		{
			auto group = GetBestGroupForWaveform(s);
			auto area = make_shared<WaveformArea>(s, group, this);
			group->AddArea(area);
		}
	}

	//Refresh any dialogs that depend on it
	RefreshTimebasePropertiesDialog();
	RefreshTriggerPropertiesDialog();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

void MainWindow::Render()
{
	//Shut down session, if requested, before starting the frame
	if(m_sessionClosing)
	{
		{
			QueueLock qlock(m_renderQueue);
			(*qlock).waitIdle();
		}
		CloseSession();
	}

	//Load all of our fonts
	UpdateFonts();

	VulkanWindow::Render();
}

void MainWindow::DoRender(vk::raii::CommandBuffer& /*cmdBuf*/)
{

}

/**
	@brief Run the tone-mapping shader on all of our waveforms

	Called by Session::CheckForWaveforms() at the start of each frame if new data is ready to render
 */
void MainWindow::ToneMapAllWaveforms(vk::raii::CommandBuffer& cmdbuf)
{
	double start = GetTime();

	lock_guard<mutex> lock(m_session.GetRasterizedWaveformMutex());

	m_cmdBuffer->begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

	//Tone map the waveforms, holding the group mutex for as short a time as possible
	vector<shared_ptr<WaveformGroup>> groups;
	{
		lock_guard<recursive_mutex> lock2(m_waveformGroupsMutex);
		groups = m_waveformGroups;
	}
	for(auto group : groups)
		group->ToneMapAllWaveforms(cmdbuf);

	m_cmdBuffer->end();
	m_renderQueue->SubmitAndBlock(*m_cmdBuffer);

	double dt = GetTime() - start;
	m_toneMapTime = dt * FS_PER_SECOND;
}

void MainWindow::RenderWaveformTextures(
	vk::raii::CommandBuffer& cmdbuf,
	vector<shared_ptr<DisplayedChannel> >& channels)
{
	bool clear = m_clearPersistence.exchange(false);
	vector<shared_ptr<WaveformGroup>> groups;
	{
		lock_guard<recursive_mutex> lock2(m_waveformGroupsMutex);
		groups = m_waveformGroups;
	}
	for(auto group : groups)
		group->RenderWaveformTextures(cmdbuf, channels, clear);
}

void MainWindow::RenderUI()
{
	//Set up colors
	switch(m_session.GetPreferences().GetEnumRaw("Appearance.General.theme"))
	{
		case THEME_LIGHT:
			ImGui::StyleColorsLight();
			break;

		case THEME_DARK:
			ImGui::StyleColorsDark();
			break;

		case THEME_CLASSIC:
			ImGui::StyleColorsClassic();
			break;
	}

	m_needRender = false;

	//Keep references to all of our waveform textures until next frame
	//Any groups we're closing will be destroyed at the start of that frame, once rendering has finished
	{
		lock_guard<recursive_mutex> lock(m_waveformGroupsMutex);
		for(auto g : m_waveformGroups)
			g->ReferenceWaveformTextures();
	}

	//Destroy all waveform groups we were asked to close
	//Block until all background processing completes to ensure no command buffers are still pending
	if(!m_groupsToClose.empty())
	{
		g_vkComputeDevice->waitIdle();
		m_groupsToClose.clear();
	}

	//Request a refresh of any dirty filters next frame
	m_session.RefreshDirtyFiltersNonblocking();

	//See if we have new waveform data to look at.
	//If we got one, highlight the new waveform in history
	if(m_session.CheckForWaveforms(*m_cmdBuffer))
	{
		if(m_historyDialog != nullptr)
			m_historyDialog->UpdateSelectionToLatest();

		//Tell protocol analyzer dialogs a new waveform arrived
		auto t = m_session.GetHistory().GetMostRecentPoint();
		for(auto it : m_protocolAnalyzerDialogs)
			it.second->OnWaveformLoaded(t);
	}

	//Menu for main window
	MainMenu();
	Toolbar();

	//Docking area to put all of the groups in
	DockingArea();

	//Waveform groups
	{
		shared_lock<shared_mutex> lock(m_session.GetWaveformDataMutex());
		lock_guard<recursive_mutex> lock2(m_waveformGroupsMutex);

		for(size_t i=0; i<m_waveformGroups.size(); i++)
		{
			auto group = m_waveformGroups[i];
			if(!group->Render())
			{
				LogTrace("Closing waveform group %s (i=%zu)\n", group->GetTitle().c_str(), i);
				group->Clear();
				m_groupsToClose.push_back(i);
			}
		}
		for(ssize_t i = static_cast<ssize_t>(m_groupsToClose.size())-1; i >= 0; i--)
			m_waveformGroups.erase(m_waveformGroups.begin() + m_groupsToClose[i]);
	}

	//Dialog boxes
	set< shared_ptr<Dialog> > dlgsToClose;
	for(auto& dlg : m_dialogs)
	{
		if(!dlg->Render())
			dlgsToClose.emplace(dlg);
	}
	for(auto& dlg : dlgsToClose)
		OnDialogClosed(dlg);

	//If we had a history dialog, check if we changed the selection
	if( (m_historyDialog != nullptr) && (m_historyDialog->PollForSelectionChanges()))
	{
		LogTrace("history selection changed\n");
		m_historyDialog->LoadHistoryFromSelection(m_session);

		auto t = m_historyDialog->GetSelectedPoint();
		if(t != TimePoint(0,0))
		{
			for(auto it : m_protocolAnalyzerDialogs)
				it.second->OnWaveformLoaded(t);
		}

		m_session.RefreshAllFiltersNonblocking();
		m_needRender = true;
	}

	//File browser dialogs
	if(m_fileBrowser)
		RenderFileBrowser();

	//Check if we changed the selected waveform from a protocol analyzer dialog
	for(auto it : m_protocolAnalyzerDialogs)
	{
		if(it.second->PollForSelectionChanges())
		{
			auto tstamp = it.second->GetSelectedWaveformTimestamp();
			auto& hist = m_session.GetHistory();
			if(m_historyDialog)
				m_historyDialog->SelectTimestamp(tstamp);

			auto hpt = hist.GetHistory(tstamp);
			if(hpt)
			{
				hpt->LoadHistoryToSession(m_session);
				m_needRender = true;
			}
			m_session.RefreshAllFiltersNonblocking();
		}
	}

	//Handle error messages
	RenderErrorPopup();
	RenderLoadWarningPopup();

	if(m_needRender)
		g_rerenderRequestedEvent.Signal();

	//DEBUG: draw the demo windows
	if(m_showDemo)
		ImGui::ShowDemoWindow(&m_showDemo);
	if(m_showPlot)
		ImPlot::ShowDemoWindow(&m_showPlot);
}

void MainWindow::Toolbar()
{
	//Update icons, if needed
	LoadToolbarIcons();

	//Toolbar should be at the top of the main window.
	//Update work area size so docking area doesn't include the toolbar rectangle
	auto viewport = ImGui::GetMainViewport();
	float toolbarHeight = m_toolbarIconSize + 8;
	m_workPos = ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + toolbarHeight);
	m_workSize = ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - toolbarHeight);
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, toolbarHeight));

	//Make the toolbar window
	auto wflags =
		ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoCollapse;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	bool open = true;
	ImGui::Begin("toolbar", &open, wflags);
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));

	//Do the actual toolbar buttons
	ToolbarButtons();

	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);

	//Slider for trace alpha
	ImGui::SameLine();
	float y = ImGui::GetCursorPosY();
	ImGui::SetCursorPosY(y + 5);
	ImGui::SetNextItemWidth(6 * toolbarHeight);
	if(ImGui::SliderFloat("Intensity", &m_traceAlpha, 0, 0.75, "", ImGuiSliderFlags_Logarithmic))
		SetNeedRender();
	ImGui::SetCursorPosY(y);

	ImGui::End();
}

/**
	@brief Load toolbar icons from disk if preferences changed
 */
void MainWindow::LoadToolbarIcons()
{
	int iconSize = m_session.GetPreferences().GetEnumRaw("Appearance.Toolbar.icon_size");

	if(m_toolbarIconSize == iconSize)
		return;

	m_toolbarIconSize = iconSize;

	string prefix = string("icons/") + to_string(iconSize) + "x" + to_string(iconSize) + "/";

	//Load the icons
	m_texmgr.LoadTexture("clear-sweeps", FindDataFile(prefix + "clear-sweeps.png"));
	m_texmgr.LoadTexture("fullscreen-enter", FindDataFile(prefix + "fullscreen-enter.png"));
	m_texmgr.LoadTexture("fullscreen-exit", FindDataFile(prefix + "fullscreen-exit.png"));
	m_texmgr.LoadTexture("history", FindDataFile(prefix + "history.png"));
	m_texmgr.LoadTexture("refresh-settings", FindDataFile(prefix + "refresh-settings.png"));
	m_texmgr.LoadTexture("trigger-single", FindDataFile(prefix + "trigger-single.png"));
	m_texmgr.LoadTexture("trigger-force", FindDataFile(prefix + "trigger-single.png"));	//no dedicated icon yet
	m_texmgr.LoadTexture("trigger-start", FindDataFile(prefix + "trigger-start.png"));
	m_texmgr.LoadTexture("trigger-stop", FindDataFile(prefix + "trigger-stop.png"));
}

/**
	@brief Load gradient images
 */
void MainWindow::LoadGradients()
{
	LogTrace("Loading eye pattern gradients...\n");
	LogIndenter li;

	LoadGradient("CRT", "eye-gradient-crt");
	LoadGradient("Grayscale", "eye-gradient-grayscale");
	LoadGradient("Ironbow", "eye-gradient-ironbow");
	LoadGradient("KRain", "eye-gradient-krain");
	LoadGradient("Rainbow", "eye-gradient-rainbow");
	LoadGradient("Reverse Rainbow", "eye-gradient-reverse-rainbow");
	LoadGradient("Viridis", "eye-gradient-viridis");
}

/**
	@brief Load a single gradient
 */
void MainWindow::LoadGradient(const string& friendlyName, const string& internalName)
{
	string prefix = string("icons/gradients/");
	m_texmgr.LoadTexture(internalName, FindDataFile(prefix + internalName + ".png"));
	m_eyeGradientFriendlyNames[internalName] = friendlyName;
	m_eyeGradients.push_back(internalName);
}

bool MainWindow::DropdownButton(const char* id, float height)
{
	auto pos = ImGui::GetCursorPos();
	auto buttonheight = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y;
	pos.y += (height - buttonheight);
	ImGui::SetCursorPos(pos);
	return ImGui::ArrowButton(id, ImGuiDir_Down);
}

void MainWindow::DoTriggerDropdown(const char* action, shared_ptr<TriggerGroup>& group, bool& all)
{
	all = false;

	ImGuiTableFlags flags =
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_BordersV |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_SizingFixedFit |
		ImGuiTableFlags_NoKeepColumnsVisible;

	if(ImGui::BeginTable("groups", 3, flags))
	{
		float width = ImGui::GetFontSize();
		ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthFixed, 4*width);
		ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 12*width);
		ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 8*width);
		ImGui::TableHeadersRow();

		auto groups = m_session.GetTriggerGroups();
		for(auto g : groups)
		{
			ImGui::PushID(g.get());
			ImGui::TableNextRow(ImGuiTableRowFlags_None);

			//Default checkbox
			ImGui::TableSetColumnIndex(0);
			ImGui::Checkbox("###default", &g->m_default);

			//Group name
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(g->GetDescription().c_str());

			//Action buttons
			ImGui::TableSetColumnIndex(2);
			if(ImGui::Button(action))
				group = g;

			ImGui::PopID();
		}

		ImGui::PushID("all");
		ImGui::TableNextRow(ImGuiTableRowFlags_None);
		ImGui::TableSetColumnIndex(1);
		ImGui::TextUnformatted("All");

		//Action buttons
		ImGui::TableSetColumnIndex(2);
		if(ImGui::Button(action))
			all = true;
		ImGui::PopID();

		ImGui::EndTable();
	}
}

void MainWindow::TriggerStartDropdown(float buttonsize)
{
	if(DropdownButton("trigger-start-dropdown", buttonsize))
		ImGui::OpenPopup("TriggerStartMenu");
	if(ImGui::BeginPopup("TriggerStartMenu"))
	{
		bool all = false;
		shared_ptr<TriggerGroup> group;
		DoTriggerDropdown("Start", group, all);

		if(group)
			group->Arm(TriggerGroup::TRIGGER_TYPE_NORMAL);
		if(all)
			m_session.ArmTrigger(TriggerGroup::TRIGGER_TYPE_NORMAL, true);

		ImGui::EndPopup();
	}
}

void MainWindow::TriggerSingleDropdown(float buttonsize)
{
	if(DropdownButton("trigger-single-dropdown", buttonsize))
		ImGui::OpenPopup("TriggerSingleMenu");
	if(ImGui::BeginPopup("TriggerSingleMenu"))
	{
		bool all = false;
		shared_ptr<TriggerGroup> group;
		DoTriggerDropdown("Single", group, all);

		//Start trigger for only a specific group
		if(group)
			group->Arm(TriggerGroup::TRIGGER_TYPE_SINGLE);

		//Start trigger for all groups
		if(all)
			m_session.ArmTrigger(TriggerGroup::TRIGGER_TYPE_SINGLE, true);

		ImGui::EndPopup();
	}
}

void MainWindow::TriggerForceDropdown(float buttonsize)
{
	if(DropdownButton("trigger-force-dropdown", buttonsize))
		ImGui::OpenPopup("TriggerForceMenu");
	if(ImGui::BeginPopup("TriggerForceMenu"))
	{
		bool all = false;
		shared_ptr<TriggerGroup> group;
		DoTriggerDropdown("Force", group, all);

		//Start trigger for only a specific group
		if(group)
			group->Arm(TriggerGroup::TRIGGER_TYPE_FORCED);

		//Start trigger for all groups
		if(all)
			m_session.ArmTrigger(TriggerGroup::TRIGGER_TYPE_FORCED, true);

		ImGui::EndPopup();
	}
}

void MainWindow::TriggerStopDropdown(float buttonsize)
{
	if(DropdownButton("trigger-stop-dropdown", buttonsize))
		ImGui::OpenPopup("TriggerStopMenu");
	if(ImGui::BeginPopup("TriggerStopMenu"))
	{
		bool all = false;
		shared_ptr<TriggerGroup> group;
		DoTriggerDropdown("Stop", group, all);

		//Start trigger for only a specific group
		if(group)
			group->Stop();

		//Stop trigger for all groups
		if(all)
			m_session.StopTrigger(true);

		ImGui::EndPopup();
	}
}

void MainWindow::ToolbarButtons()
{
	ImVec2 buttonsize(m_toolbarIconSize, m_toolbarIconSize);

	bool multigroup = (m_session.GetTriggerGroups().size() > 1);

	//Trigger button group
	if(ImGui::ImageButton("trigger-start", GetTexture("trigger-start"), buttonsize))
		m_session.ArmTrigger(TriggerGroup::TRIGGER_TYPE_NORMAL);
	Dialog::Tooltip("Arm the trigger in normal mode");
	if(multigroup)
	{
		ImGui::SameLine(0.0, 0.0);
		TriggerStartDropdown(buttonsize.y);
	}

	ImGui::SameLine(0.0, 0.0);
	if(ImGui::ImageButton("trigger-single", GetTexture("trigger-single"), buttonsize))
		m_session.ArmTrigger(TriggerGroup::TRIGGER_TYPE_SINGLE);
	Dialog::Tooltip("Arm the trigger in one-shot mode");
	if(multigroup)
	{
		ImGui::SameLine(0.0, 0.0);
		TriggerSingleDropdown(buttonsize.y);
	}

	ImGui::SameLine(0.0, 0.0);
	if(ImGui::ImageButton("trigger-force", GetTexture("trigger-force"), buttonsize))
		m_session.ArmTrigger(TriggerGroup::TRIGGER_TYPE_FORCED);
	Dialog::Tooltip("Acquire a waveform immediately, ignoring the trigger condition");
	if(multigroup)
	{
		ImGui::SameLine(0.0, 0.0);
		TriggerForceDropdown(buttonsize.y);
	}

	ImGui::SameLine(0.0, 0.0);
	if(ImGui::ImageButton("trigger-stop", GetTexture("trigger-stop"), buttonsize))
		m_session.StopTrigger();
	Dialog::Tooltip("Stop acquiring waveforms");
	if(multigroup)
	{
		ImGui::SameLine(0.0, 0.0);
		TriggerStopDropdown(buttonsize.y);
	}

	//History selector
	bool hasHist = (m_historyDialog != nullptr);
	ImGui::SameLine();
	if(hasHist)
		ImGui::BeginDisabled();
	if(ImGui::ImageButton("history", GetTexture("history"), buttonsize))
	{
		m_historyDialog = make_shared<HistoryDialog>(m_session.GetHistory(), m_session, *this);
		AddDialog(m_historyDialog);
	}
	if(hasHist)
		ImGui::EndDisabled();
	Dialog::Tooltip("Show waveform history window");

	//Refresh scope settings
	ImGui::SameLine();
	if(ImGui::ImageButton("refresh-settings", GetTexture("refresh-settings"), buttonsize))
		LogDebug("refresh settings\n");
	Dialog::Tooltip(
		"Flush PC-side cached instrument state and reload configuration from the instrument.\n\n"
		"This will cause a brief slowdown of the application, but can be used to re-sync when\n"
		"changes are made on the instrument front panel that ngscopeclient does not detect."
		);

	//View settings
	ImGui::SameLine();
	if(ImGui::ImageButton("clear-sweeps", GetTexture("clear-sweeps"), buttonsize))
	{
		ClearPersistence();
		m_session.ClearSweeps();
	}
	Dialog::Tooltip("Clear waveform persistence, eye patterns, and accumulated filter block state");

	//Fullscreen toggle
	ImGui::SameLine(0.0, 0.0);
	if(m_fullscreen)
	{
		if(ImGui::ImageButton("fullscreen-exit", GetTexture("fullscreen-exit"), buttonsize))
			SetFullscreen(false);
		Dialog::Tooltip("Leave fullscreen mode");
	}
	else
	{
		if(ImGui::ImageButton("fullscreen-enter", GetTexture("fullscreen-enter"), buttonsize))
			SetFullscreen(true);
		Dialog::Tooltip("Enter fullscreen mode");
	}
}

void MainWindow::OnDialogClosed(const std::shared_ptr<Dialog>& dlg)
{
	//Handle multi-instance dialogs
	auto meterDlg = dynamic_pointer_cast<MultimeterDialog>(dlg);
	if(meterDlg)
		m_meterDialogs.erase(meterDlg->GetMeter());

	auto psuDlg = dynamic_pointer_cast<PowerSupplyDialog>(dlg);
	if(psuDlg)
		m_psuDialogs.erase(psuDlg->GetPSU());

	auto genDlg = dynamic_pointer_cast<FunctionGeneratorDialog>(dlg);
	if(genDlg)
		m_generatorDialogs.erase(genDlg->GetGenerator());

	auto rgenDlg = dynamic_pointer_cast<RFGeneratorDialog>(dlg);
	if(rgenDlg)
		m_rfgeneratorDialogs.erase(rgenDlg->GetGenerator());

	auto bertDlg = dynamic_pointer_cast<BERTDialog>(dlg);
	if(bertDlg)
		m_bertDialogs.erase(bertDlg->GetBERT());

	auto loadDlg = dynamic_pointer_cast<LoadDialog>(dlg);
	if(loadDlg)
		m_loadDialogs.erase(loadDlg->GetLoad());

	auto conDlg = dynamic_pointer_cast<SCPIConsoleDialog>(dlg);
	if(conDlg)
		m_scpiConsoleDialogs.erase(conDlg->GetInstrument());

	auto chanDlg = dynamic_pointer_cast<ChannelPropertiesDialog>(dlg);
	if(chanDlg)
		m_channelPropertiesDialogs.erase(chanDlg->GetChannel());

	auto protoDlg = dynamic_pointer_cast<ProtocolAnalyzerDialog>(dlg);
	if(protoDlg)
		m_protocolAnalyzerDialogs.erase(protoDlg->GetFilter());

	//Handle single-instance dialogs
	if(m_logViewerDialog == dlg)
		m_logViewerDialog = nullptr;
	if(m_timebaseDialog == dlg)
		m_timebaseDialog = nullptr;
	if(m_triggerDialog == dlg)
		m_triggerDialog = nullptr;
	if(m_preferenceDialog == dlg)
		m_preferenceDialog = nullptr;
	if(m_persistenceDialog == dlg)
		m_persistenceDialog = nullptr;
	if(m_graphEditor == dlg)
		m_graphEditor = nullptr;
	if(m_measurementsDialog == dlg)
		m_measurementsDialog = nullptr;

	//Remove the general list
	m_dialogs.erase(dlg);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Waveform views etc

void MainWindow::DockingArea()
{
	//Provide a space we can dock windows into
	auto viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(m_workPos);
	ImGui::SetNextWindowSize(m_workSize);
	ImGui::SetNextWindowViewport(viewport->ID);

	ImGuiWindowFlags host_window_flags = 0;
	host_window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
	host_window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

	char label[32];
	ImFormatString(label, IM_ARRAYSIZE(label), "DockSpaceViewport_%08X", viewport->ID);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin(label, NULL, host_window_flags);
	ImGui::PopStyleVar(3);

	auto dockspace_id = ImGui::GetID("DockSpace");

	//Handle splitting of existing waveform groups
	if(!m_splitRequests.empty())
	{
		LogTrace("Processing split request\n");

		for(auto& request : m_splitRequests)
		{
			//Get the window for the group
			auto window = ImGui::FindWindowByName(request.m_group->GetTitle().c_str());
			if(!window)
			{
				//Not sure if this is possible? Haven't seen it yet
				LogWarning("Window is null (TODO handle this)\n");
				continue;
			}
			if(!window->DockNode)
			{
				//If we get here, we dragged into a floating window without a dock space in it
				LogWarning("Dock node is null (TODO handle this)\n");
				continue;
			}

			auto dockid = window->DockId;

			//Split the existing node
			ImGuiID idA;
			ImGuiID idB;
			ImGui::DockBuilderSplitNode(dockid, request.m_direction, 0.5, &idA, &idB);
			auto node = ImGui::DockBuilderGetNode(idA);

			//Create a new waveform group and dock it into the new space
			auto group = make_shared<WaveformGroup>(this, NameNewWaveformGroup());
			{
				lock_guard<recursive_mutex> lock(m_waveformGroupsMutex);
				m_waveformGroups.push_back(group);
			}
			ImGui::DockBuilderDockWindow(group->GetTitle().c_str(), node->ID);

			//Add a new waveform area for our stream to the new group
			auto area = make_shared<WaveformArea>(request.m_stream, group, this);
			group->AddArea(area);
		}

		//Finish up
		ImGui::DockBuilderFinish(dockspace_id);

		m_splitRequests.clear();
	}

	//Handle newly created waveform groups
	//Do not do this the same frame as split requests
	else if(!m_newWaveformGroups.empty())
	{
		LogTrace("Processing newly added waveform group\n");

		//Find the top/leftmost leaf node in the docking tree
		auto topNode = ImGui::DockBuilderGetNode(dockspace_id);
		if(topNode == nullptr)
		{
			LogError("Top dock node is null when adding new waveform group\n");
			return;
		}

		//Traverse down the top/left of the tree as long as such a node exists
		auto node = topNode;
		while(node->ChildNodes[0])
			node = node->ChildNodes[0];

		//See if the node has children in it
		if(!node->Windows.empty())
		{
			LogTrace("Windows already in node, splitting it\n");
			ImGuiID idLeft;
			ImGuiID idRight;

			ImGui::DockBuilderSplitNode(node->ID, ImGuiDir_Up, 0.5, &idLeft, &idRight);
			node = ImGui::DockBuilderGetNode(idLeft);
		}

		//Dock new waveform groups by default
		for(auto& g : m_newWaveformGroups)
			ImGui::DockBuilderDockWindow(g->GetTitle().c_str(), node->ID);

		//Finish up
		ImGui::DockBuilderFinish(dockspace_id);

		//Everything pending has been docked, no need to do anything with them in the future
		m_newWaveformGroups.clear();
	}

	ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), /*dockspace_flags*/0, /*window_class*/nullptr);
	ImGui::End();
}

/**
	@brief Scrolls all waveform groups so that the specified timestamp is visible
 */
void MainWindow::NavigateToTimestamp(int64_t stamp, int64_t duration, StreamDescriptor target)
{
	lock_guard<recursive_mutex> lock(m_waveformGroupsMutex);
	for(auto group : m_waveformGroups)
		group->NavigateToTimestamp(stamp, duration, target);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Other GUI handlers

/**
	@brief Returns true if a channel is being dragged from any WaveformArea within this window
 */
bool MainWindow::IsChannelBeingDragged()
{
	lock_guard<recursive_mutex> lock(m_waveformGroupsMutex);
	for(auto group : m_waveformGroups)
	{
		if(group->IsChannelBeingDragged())
			return true;
	}
	return false;
}

/**
	@brief Returns the channel being dragged, if one exists
 */
StreamDescriptor MainWindow::GetChannelBeingDragged()
{
	lock_guard<recursive_mutex> lock(m_waveformGroupsMutex);
	for(auto group : m_waveformGroups)
	{
		auto stream = group->GetChannelBeingDragged();
		if(stream)
			return stream;
	}
	return StreamDescriptor(nullptr, 0);
}

void MainWindow::ShowTimebaseProperties()
{
	if(m_timebaseDialog != nullptr)
		return;

	m_timebaseDialog = make_shared<TimebasePropertiesDialog>(&m_session);
	AddDialog(m_timebaseDialog);
}

void MainWindow::ShowSyncWizard(shared_ptr<TriggerGroup> group, Oscilloscope* secondary)
{
	AddDialog(make_shared<ScopeDeskewWizard>(group, secondary, this, m_session));
}

void MainWindow::ShowManageInstruments()
{
	if(m_manageInstrumentsDialog != nullptr)
		return;

	m_manageInstrumentsDialog = make_shared<ManageInstrumentsDialog>(m_session, this);
	AddDialog(m_manageInstrumentsDialog);
}

void MainWindow::ShowTriggerProperties()
{
	if(m_triggerDialog != nullptr)
		return;

	m_triggerDialog = make_shared<TriggerPropertiesDialog>(&m_session);
	AddDialog(m_triggerDialog);
}

void MainWindow::ShowChannelProperties(OscilloscopeChannel* channel)
{
	LogTrace("Show properties for %s\n", channel->GetHwname().c_str());
	LogIndenter li;

	if(m_channelPropertiesDialogs.find(channel) != m_channelPropertiesDialogs.end())
	{
		LogTrace("Properties dialog is already open, no action required\n");
		return;
	}

	//Dialog wasn't already open, create it
	auto f = dynamic_cast<Filter*>(channel);
	auto bi = dynamic_cast<BERTInputChannel*>(channel);
	if(bi)
	{
		auto dlg = make_shared<BERTInputChannelDialog>(bi, this);
		m_channelPropertiesDialogs[channel] = dlg;
		AddDialog(dlg);
	}
	else if(f)
	{
		auto dlg = make_shared<FilterPropertiesDialog>(f, this);
		m_channelPropertiesDialogs[channel] = dlg;
		AddDialog(dlg);
	}
	else
	{
		auto dlg = make_shared<ChannelPropertiesDialog>(channel);
		m_channelPropertiesDialogs[channel] = dlg;
		AddDialog(dlg);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Recent instruments

void MainWindow::LoadRecentInstrumentList()
{
	try
	{
		auto docs = YAML::LoadAllFromFile(m_session.GetPreferences().GetConfigDirectory() + "/recent.yml");
		if(docs.empty())
			return;
		auto node = docs[0];

		for(auto it : node)
		{
			auto inst = it.second;
			m_recentInstruments[inst["path"].as<string>()] = inst["timestamp"].as<long long>();
		}
	}
	catch(const YAML::BadFile& ex)
	{
		LogDebug("Unable to open recently used instruments file\n");
		return;
	}

}

void MainWindow::SaveRecentInstrumentList()
{
	LogTrace("Saving recent instrument list\n");

	auto path = m_session.GetPreferences().GetConfigDirectory() + "/recent.yml";
	FILE* fp = fopen(path.c_str(), "w");

	for(auto it : m_recentInstruments)
	{
		auto nick = it.first.substr(0, it.first.find(":"));
		fprintf(fp, "%s:\n", nick.c_str());
		fprintf(fp, "    path: \"%s\"\n", it.first.c_str());
		fprintf(fp, "    timestamp: %ld\n", it.second);
	}

	fclose(fp);
}

void MainWindow::AddToRecentInstrumentList(SCPIInstrument* inst)
{
	if(inst == nullptr)
		return;

	LogTrace("Adding instrument \"%s\" to recent instrument list\n", inst->m_nickname.c_str());

	auto now = time(NULL);

	auto connectionString =
		inst->m_nickname + ":" +
		inst->GetDriverName() + ":" +
		inst->GetTransportName() + ":" +
		inst->GetTransportConnectionString();
	m_recentInstruments[connectionString] = now;

	//Delete anything old
	size_t maxRecentInstruments = m_session.GetPreferences().GetInt("Miscellaneous.Menus.recent_instrument_count");
	while(m_recentInstruments.size() > maxRecentInstruments)
	{
		string oldestPath = "";
		time_t oldestTime = now;

		for(auto it : m_recentInstruments)
		{
			if(it.second < oldestTime)
			{
				oldestTime = it.second;
				oldestPath = it.first;
			}
		}

		m_recentInstruments.erase(oldestPath);
	}
}

/**
	@brief Helper function for creating a transport and printing an error if the connection is unsuccessful
 */
SCPITransport* MainWindow::MakeTransport(const string& trans, const string& args)
{
	//Create the transport
	auto transport = SCPITransport::CreateTransport(trans, args);
	if(transport == nullptr)
	{
		ShowErrorPopup(
			"Transport error",
			"Failed to create transport of type \"" + trans + "\"");
		return nullptr;
	}

	//Make sure we connected OK
	if(!transport->IsConnected())
	{
		delete transport;
		ShowErrorPopup("Connection error", "Failed to connect to \"" + args + "\"");
		return nullptr;
	}

	return transport;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Dialog helpers

shared_ptr<MeasurementsDialog> MainWindow::GetMeasurementsDialog(bool createIfNotExisting)
{
	if(m_measurementsDialog)
		return m_measurementsDialog;
	else if(createIfNotExisting)
	{
		m_measurementsDialog = make_shared<MeasurementsDialog>(m_session);
		AddDialog(m_measurementsDialog);
		return m_measurementsDialog;
	}
	else
		return nullptr;
}

/**
	@brief Opens the error popup
 */
void MainWindow::ShowErrorPopup(const string& title, const string& msg)
{
	m_errorPopupTitle = title;
	m_errorPopupMessage = msg;
}

/**
	@brief Popup message when something big goes wrong
 */
void MainWindow::RenderErrorPopup()
{
	if(!m_errorPopupTitle.empty())
		ImGui::OpenPopup(m_errorPopupTitle.c_str());

	if(ImGui::BeginPopupModal(m_errorPopupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted(m_errorPopupMessage.c_str());
		ImGui::Separator();
		if(ImGui::Button("OK"))
		{
			m_errorPopupMessage = "";
			m_errorPopupTitle = "";
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

/**
	@brief Popup message when loading a file that might not match the current hardware setup
 */
void MainWindow::RenderLoadWarningPopup()
{
	static ImGuiTableFlags flags =
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_BordersV |
		//ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_RowBg |
		ImGuiTableFlags_SizingFixedFit;

	float width = ImGui::GetFontSize();
	float warningSize = ImGui::GetFontSize() * 3;

	const char* title = "WARNING: Potential for hardware damage!";

	if(m_showingLoadWarnings)
		ImGui::OpenPopup(title);

	if(ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::PushTextWrapPos(40*width);

		ImGui::Image(
			GetTextureManager()->GetTexture("warning"),
			ImVec2(warningSize, warningSize));
		ImGui::SameLine();
		ImGui::TextUnformatted(
			"Some of the instrument settings in the session you are loading do not match "
			"the current hardware configuration, and if set incorrectly could potentially "
			"damage the instrument and/or DUT."
			);

		ImGui::PopTextWrapPos();

		ImGui::NewLine();

		if(ImGui::BeginTable("table", 5, flags))
		{
			//Header row
			ImGui::TableSetupScrollFreeze(0, 1); //Header row does not scroll
			ImGui::TableSetupColumn("Instrument", ImGuiTableColumnFlags_WidthFixed, 5*width);
			ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthFixed, 12*width);
			ImGui::TableSetupColumn("Hardware", ImGuiTableColumnFlags_WidthFixed, 5*width);
			ImGui::TableSetupColumn("Session file", ImGuiTableColumnFlags_WidthFixed, 5*width);
			ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthFixed, 40*width);
			ImGui::TableHeadersRow();

			//Actual list of warnings
			auto& warnings = m_session.GetWarnings();
			for(auto it : warnings.m_warnings)
			{
				ImGui::PushID(it.first);

				for(auto w : it.second.m_messages)
				{
					ImGui::TableNextRow(ImGuiTableRowFlags_None);

					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(it.first->m_nickname.c_str());

					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(w.m_object.c_str());

					ImGui::TableSetColumnIndex(2);
					ImGui::TextUnformatted(w.m_existingValue.c_str());

					ImGui::TableSetColumnIndex(3);
					ImGui::TextUnformatted(w.m_proposedValue.c_str());

					ImGui::TableSetColumnIndex(4);
					ImGui::TextUnformatted(w.m_messageText.c_str());
				}

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::NewLine();
		ImGui::Separator();

		ImGui::Checkbox("I have reviewed the settings to be applied and confirmed they will not cause damage.",
			&m_loadConfirmationChecked);

		if(ImGui::Button("Abort"))
		{
			CloseSession();
			m_showingLoadWarnings = false;
			m_fileLoadInProgress = false;

			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if(!m_loadConfirmationChecked)
			ImGui::BeginDisabled();

		if(ImGui::Button("Proceed"))
		{
			//Continue with the load
			//always loading online if we are warning, offline loads can't warn)
			if(LoadSessionFromYaml(m_fileBeingLoaded[0], m_sessionDataDir, true))
			{
				m_recentFiles[m_sessionFileName] = time(nullptr);
				SaveRecentFileList();
			}
			else
				CloseSession();

			m_showingLoadWarnings = false;
			m_fileLoadInProgress = false;

			ImGui::CloseCurrentPopup();
		}

		if(!m_loadConfirmationChecked)
			ImGui::EndDisabled();

		ImGui::EndPopup();
	}
}

/**
	@brief Closes the function generator dialog, if we have one
 */
void MainWindow::RemoveFunctionGenerator(SCPIFunctionGenerator* gen)
{
	auto it = m_generatorDialogs.find(gen);
	if(it != m_generatorDialogs.end())
	{
		m_generatorDialogs.erase(gen);
		m_dialogs.erase(it->second);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Font handling

/**
	@brief Font
 */
void MainWindow::UpdateFonts()
{
	//Check for any changes to font preferences and rebuild the atlas if so
	//Early out if nothing changed
	auto& prefs = GetSession().GetPreferences();
	if(!m_fontmgr.UpdateFonts(prefs.AllPreferences(), GetContentScale()))
		return;

	//Set the default font
	ImGui::GetIO().FontDefault = m_fontmgr.GetFont(prefs.GetFont("Appearance.General.default_font"));

	//Download imgui fonts
	m_cmdBuffer->begin({});
	ImGui_ImplVulkan_CreateFontsTexture(**m_cmdBuffer);
	m_cmdBuffer->end();
	m_renderQueue->SubmitAndBlock(*m_cmdBuffer);
	ImGui_ImplVulkan_DestroyFontUploadObjects();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Filter creation etc

/**
	@brief Creates a filter optionally and adds all of its streams to the best waveform area

	@param name				Name of the filter
	@param area				Waveform area we launched the context menu from (if any)
	@param initialStream	Stream we launched the context menu from (if any)
	@param showProperties	True to show the properties dialog
	@param addtoArea		True to add to a waveform area
 */
Filter* MainWindow::CreateFilter(
	const string& name,
	WaveformArea* area,
	StreamDescriptor initialStream,
	bool showProperties,
	bool addToArea)
{
	LogTrace("CreateFilter %s\n", name.c_str());

	//Make sure we have a WaveformThread to handle background processing
	m_session.StartWaveformThreadIfNeeded();

	//Make the filter
	auto f = Filter::CreateFilter(name, GetDefaultChannelColor(Filter::GetNumInstances()));

	//Attempt to hook up first input
	if(f->ValidateChannel(0, initialStream))
		f->SetInput(0, initialStream);

	//Give it an initial name, may change later
	f->SetDefaultName();

	//Re-run the filter graph so we have an initial waveform to look at
	m_session.RefreshAllFiltersNonblocking();

	//Find a home for each of its streams
	if(addToArea)
	{
		for(size_t i=0; i<f->GetStreamCount(); i++)
			FindAreaForStream(area, StreamDescriptor(f, i));
	}

	//Not adding waveforms to plots, but still check for scalar values and add to measurements view
	else
	{
		for(size_t i=0; i<f->GetStreamCount(); i++)
		{
			StreamDescriptor stream(f, i);
			if(stream.GetType() == Stream::STREAM_TYPE_ANALOG_SCALAR)
				FindAreaForStream(area, stream);
		}
	}

	//Create and show filter properties dialog
	if( (f->NeedsConfig() && showProperties) || (f->GetCategory() == Filter::CAT_GENERATION))
	{
		auto dlg = make_shared<FilterPropertiesDialog>(f, this);
		m_channelPropertiesDialogs[f] = dlg;
		AddDialog(dlg);
		dlg->SpawnFileDialogForImportFilter();
	}

	//Create and show protocol analyzer dialog
	auto pd = dynamic_cast<PacketDecoder*>(f);
	if(pd)
	{
		m_session.AddPacketFilter(pd);

		auto dlg = make_shared<ProtocolAnalyzerDialog>(pd, m_session.GetPacketManager(pd), m_session, *this);
		m_protocolAnalyzerDialogs[pd] = dlg;
		AddDialog(dlg);
	}

	//If the filter is pausable, add to the default trigger group for that
	auto pf = dynamic_cast<PausableFilter*>(f);
	if(pf)
		m_session.GetTrendFilterGroup()->AddFilter(pf);

	return f;
}

/**
	@brief Given a stream and optionally a WaveformArea, adds the stream to some area.

	The provided area is considered first; if it's not a good fit then another area is selected. If no compatible
	area can be found, a new one is created.
 */
void MainWindow::FindAreaForStream(WaveformArea* area, StreamDescriptor stream)
{
	lock_guard<recursive_mutex> lock(m_waveformGroupsMutex);

	LogTrace("Looking for area for stream %s\n", stream.GetName().c_str());
	LogIndenter li;

	//If it's a scalar, add to the measurements dialog (creating it if necessary)
	if(stream.GetType() == Stream::STREAM_TYPE_ANALOG_SCALAR)
	{
		//Don't add infrequently used values by default
		if(stream.GetFlags() & Stream::STREAM_INFREQUENTLY_USED)
			return;

		LogTrace("It's a scalar, adding to measurements\n");
		if(m_measurementsDialog == nullptr)
		{
			m_measurementsDialog = make_shared<MeasurementsDialog>(m_session);
			AddDialog(m_measurementsDialog);
		}
		m_measurementsDialog->AddStream(stream);
		return;
	}

	//If it's an eye pattern, it automatically gets a new group
	bool makeNewGroup = false;
	if(stream.GetType() == Stream::STREAM_TYPE_EYE)
	{
		LogTrace("It's an eye pattern, automatic new group\n");
		makeNewGroup = true;
	}

	//No areas?
	if(m_waveformGroups.empty())
	{
		LogTrace("No waveform groups, making a new one\n");
		makeNewGroup = true;
	}

	if(makeNewGroup)
	{
		//Make it
		auto name = NameNewWaveformGroup();
		auto group = make_shared<WaveformGroup>(this, name);
		m_waveformGroups.push_back(group);

		//Group is newly created and not yet docked
		m_newWaveformGroups.push_back(group);

		//Make an area
		auto a = make_shared<WaveformArea>(stream, group, this);
		group->AddArea(a);
		return;
	}

	//TODO: how to handle Y axis scale if it doesn's match the group we decide to add it to?

	//Attempt to place close to the existing area, if one was suggested
	if(area != nullptr)
	{
		//If a suggested area was provided, try it first
		if(area->IsCompatible(stream))
		{
			LogTrace("Suggested area looks good\n");
			area->AddStream(stream);
			return;
		}

		//If X axis unit is compatible, but not Y, make a new area in the same group
		auto group = area->GetGroup();
		if(group->GetXAxisUnit() == stream.GetXAxisUnits())
		{
			LogTrace("Making new area in suggested group\n");
			auto a = make_shared<WaveformArea>(stream, group, this);
			group->AddArea(a);
			return;
		}
	}

	//If it's a filter, attempt to place on top of any compatible WaveformArea displaying our first (non-null) input
	auto f = dynamic_cast<Filter*>(stream.m_channel);
	if(f)
	{
		//Find first input that has something hooked up
		StreamDescriptor firstInput(nullptr, 0);
		for(size_t i=0; i<f->GetInputCount(); i++)
		{
			firstInput = f->GetInput(i);
			if(firstInput)
				break;
		}

		//If at least one input is hooked up, see what we can do
		if(firstInput)
		{
			for(auto g : m_waveformGroups)
			{
				//Try each area within the group
				auto areas = g->GetWaveformAreas();
				for(auto a : areas)
				{
					if(!a->IsCompatible(stream))
						continue;

					for(size_t i=0; i<a->GetStreamCount(); i++)
					{
						if(firstInput == a->GetStream(i))
						{
							LogTrace("Adding to an area that was already displaying %s\n",
								firstInput.GetName().c_str());
							a->AddStream(stream);
							return;
						}
					}
				}
			}
		}
	}

	//Try all of our other areas and see if any of them fit
	for(auto g : m_waveformGroups)
	{
		//Try each area within the group
		auto areas = g->GetWaveformAreas();
		for(auto a : areas)
		{
			if(a->IsCompatible(stream))
			{
				LogTrace("Adding to existing area in different group\n");
				a->AddStream(stream);
				return;
			}
		}

		//Try making new area in the group
		if(g->GetXAxisUnit() == stream.GetXAxisUnits())
		{
			LogTrace("Making new area in a different group\n");
			auto a = make_shared<WaveformArea>(stream, g, this);
			g->AddArea(a);
			return;
		}
	}

	//If we get here, we've run out of options so we have to make a new group
	LogTrace("Gave up on finding something good, making a new group\n");

	//Make it
	auto name = NameNewWaveformGroup();
	auto group = make_shared<WaveformGroup>(this, name);
	m_waveformGroups.push_back(group);

	//Group is newly created and not yet docked
	m_newWaveformGroups.push_back(group);

	//Make an area
	auto a = make_shared<WaveformArea>(stream, group, this);
	group->AddArea(a);
}

/**
	@brief Handle a filter being reconfigured

	TODO: push this to a background thread to avoid hanging the UI thread
 */
void MainWindow::OnFilterReconfigured(Filter* f)
{
	//Remove any saved configuration, eye patterns, etc
	{
		lock_guard lock(m_session.GetWaveformDataMutex());
		f->ClearSweeps();
	}

	//Re-run the filter
	m_session.RefreshAllFiltersNonblocking();

	//Clear persistence of any waveform areas showing this waveform
	lock_guard<recursive_mutex> lock(m_waveformGroupsMutex);
	for(auto g : m_waveformGroups)
		g->ClearPersistenceOfChannel(f);
}

/**
	@brief Called when a cursor is moved, so protocol analyzers can move highlights as needed
 */
void MainWindow::OnCursorMoved(int64_t offset)
{
	for(auto it : m_protocolAnalyzerDialogs)
		it.second->OnCursorMoved(offset);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Serialization and file load/save/export UI

/**
	@brief Handler for file | open menu. Spawns the browser dialog.
 */
void MainWindow::OnOpenFile(bool online)
{
	m_openOnline = online;
	m_fileBrowserMode = BROWSE_OPEN_SESSION;
	m_fileBrowser = MakeFileBrowser(
		this,
		".",
		"Open Session",
		"Session files (*.scopesession)",
		"*.scopesession",
		false);
}

/**
	@brief Handler for file | save as menu. Spawns the browser dialog
 */
void MainWindow::OnSaveAs()
{
	m_fileBrowserMode = BROWSE_SAVE_SESSION;
	m_fileBrowser = MakeFileBrowser(
		this,
		".",
		"Save Session",
		"Session files (*.scopesession)",
		"*.scopesession",
		true);
}

/**
	@brief Runs the file browser dialog
 */
void MainWindow::RenderFileBrowser()
{
	m_fileBrowser->Render();

	if(m_fileBrowser->IsClosed())
	{
		if(m_fileBrowser->IsClosedOK())
		{
			//A file was selected, actually execute the load/save operation
			switch(m_fileBrowserMode)
			{
				case BROWSE_OPEN_SESSION:
					DoOpenFile(m_fileBrowser->GetFileName(), m_openOnline);
					break;

				case BROWSE_SAVE_SESSION:
					DoSaveFile(m_fileBrowser->GetFileName());
					break;
			}
		}

		//Done, clean up
		m_fileBrowser = nullptr;
	}
}

/**
	@brief Actually open a file (may be triggered by dialog, command line request, or recent file menu)
 */
void MainWindow::DoOpenFile(const string& sessionPath, bool online)
{
	//Close any existing session
	CloseSession();

	//Get the data directory for the session
	string base = sessionPath.substr(0, sessionPath.length() - strlen(".scopesession"));
	string datadir = base + "_data";

	LogDebug("Opening session file \"%s\" (data directory %s)\n", sessionPath.c_str(), datadir.c_str());
	try
	{
		//Load all YAML
		m_fileBeingLoaded = YAML::LoadAllFromFile(sessionPath);
		if(m_fileBeingLoaded.size() != 1)
		{
			ShowErrorPopup(
				"File loading error",
				string("Could not load the file \"") + sessionPath + "\"!\n\n" +
				"The file may not be in .scopesession format, or may have been corrupted.\n\n" +
				"YAML parsing successful, but expected one document and found " + to_string(m_fileBeingLoaded.size()) + " instead.");
			return;
		}

		//Save file path immediately
		m_sessionFileName = sessionPath;
		m_sessionDataDir = datadir;

		//Run preload first, error out if this fails
		if(!PreLoadSessionFromYaml(m_fileBeingLoaded[0], m_sessionDataDir, online))
			m_fileLoadInProgress = false;

		//Preload successful
		else
		{
			//Preload completed with no warnings, or loading offline? Commit now
			if(!online || m_session.GetWarnings().empty())
			{
				if(LoadSessionFromYaml(m_fileBeingLoaded[0], m_sessionDataDir, online))
				{
					m_recentFiles[sessionPath] = time(nullptr);
					SaveRecentFileList();
				}

				//Loading failed, clean up any half-loaded stuff
				//Do not print any error message; LoadSessionFromYaml() is responsible for calling ShowErrorPopup()
				//if something goes wrong there.
				else
					CloseSession();
			}

			//Preload generated warnings, pop up confirmation dialog
			else
			{
				m_fileLoadInProgress = true;
				m_showingLoadWarnings = true;
				m_loadConfirmationChecked = false;
			}
		}
	}
	catch(const YAML::BadFile& ex)
	{
		LogTrace("yaml badfile\n");

		ShowErrorPopup("Cannot open file", string("Unable to open the file \"") + sessionPath + "\"!");
		return;
	}
	catch(const YAML::Exception& ex)
	{
		LogTrace("yaml exception\n");

		ShowErrorPopup(
			"File loading error",
			string("Could not load the file \"") + sessionPath + "\"!\n\n" +
			"The file may not be in .scopesession format, or may have been corrupted.\n\n" +
			"Debug information:\n" +
			ex.what());
		return;
	}
}

/**
	@brief Sanity check a YAML::Node (and associated data directory) to the current session without fully loading

	@param node		Root YAML node of the file
	@param dataDir	Path to the _data directory associated with the session
	@param online	True if we should reconnect to instruments

	@return			True if successful, false on error
 */
bool MainWindow::PreLoadSessionFromYaml(const YAML::Node& node, const string& dataDir, bool online)
{
	//Load imgui_node_editor settings first (before creating the session)
	ifstream ifs(dataDir + "/filtergraph.json");
	if(ifs)
	{
		ifs >> m_graphEditorConfigBlob;
		ifs.close();
	}

	if(!m_session.PreLoadFromYaml(node, dataDir, online))
	{
		//If loading fails, clean up any incomplete half-loaded stuff that might be in a bad state
		CloseSession();
		return false;
	}

	return true;
}

/**
	@brief Deserialize a YAML::Node (and associated data directory) to the current session

	You must call PreLoadSessionFromYaml before calling this function.

	@param node		Root YAML node of the file
	@param dataDir	Path to the _data directory associated with the session
	@param online	True if we should reconnect to instruments

	@return			True if successful, false on error
 */
bool MainWindow::LoadSessionFromYaml(const YAML::Node& node, const string& dataDir, bool online)
{
	if(!m_session.LoadFromYaml(node, dataDir, online))
	{
		//If loading fails, clean up any incomplete half-loaded stuff that might be in a bad state
		CloseSession();
		return false;
	}

	//Update all of our instrument dialogs as needed
	for(auto it : m_psuDialogs)
	{
		auto pdlg = dynamic_pointer_cast<PowerSupplyDialog>(it.second);
		if(pdlg)
			pdlg->RefreshFromHardware();
	}
	for(auto it : m_rfgeneratorDialogs)
	{
		auto rdlg = dynamic_pointer_cast<RFGeneratorDialog>(it.second);
		if(rdlg)
			rdlg->RefreshFromHardware();
	}
	for(auto it : m_bertDialogs)
	{
		auto bdlg = dynamic_pointer_cast<BERTDialog>(it.second);
		if(bdlg)
			bdlg->RefreshFromHardware();
	}
	for(auto it : m_loadDialogs)
	{
		auto ldlg = dynamic_pointer_cast<LoadDialog>(it.second);
		if(ldlg)
			ldlg->RefreshFromHardware();
	}

	//Load ImGui configuration
	LogTrace("Loading ImGui configuration\n");
	string ipath = dataDir + "/imgui.ini";
	ImGui::LoadIniSettingsFromDisk(ipath.c_str());

	LogTrace("Load completed successfully\n");
	return true;
}

bool MainWindow::LoadUIConfiguration(int version, const YAML::Node& node)
{
	LogTrace("Loading UI configuration\n");
	LogIndenter li;

	//imgui does NOT handle window height/width of the top level, only child windows
	auto window = node["window"];
	if(window)
	{
		//if fullscreening ignore width/height in file
		//as our monitor resolution may be different
		if(window["fullscreen"] && window["fullscreen"].as<bool>())
			SetFullscreen(true);

		else
		{
			m_pendingWidth = window["width"].as<int>();
			m_pendingHeight = window["height"].as<int>();
			m_softwareResizeRequested = true;
		}
	}

	//Waveform groups
	auto groups = node["groups"];
	auto areas = node["areas"];
	for(auto it : groups)
	{
		//Create the group
		auto gn = it.second;
		auto gname = gn["name"].as<string>();
		LogTrace("Creating group %s\n", gname.c_str());
		auto group = make_shared<WaveformGroup>(this, gname);
		m_waveformGroups.push_back(group);

		//Legacy file with no imgui config? auto dock the group next render
		if(version < 2)
			m_newWaveformGroups.push_back(group);

		if(!group->LoadConfiguration(gn))
		{
			LogTrace("group loading failed\n");
			return false;
		}

		//Waveform areas
		auto gareas = gn["areas"];
		for(auto at : gareas)
		{
			//Load the area here (rather than by parsing the areas node as in glscopeclient)
			//since ngscopeclient requires areas to be part of a group
			auto aid = at.second["id"].as<int>();
			LogTrace("Waveform area %d\n", aid);

			if(version < 2)
			{
				//glscopeclient pre yaml-cpp refactor doesn't have named areas, need to bruteforce search for ID match
				if(version == 0)
				{
					for(auto kt : areas)
					{
						auto an = kt.second;
						if(an["id"].as<int>() == aid)
						{
							auto channel = static_cast<OscilloscopeChannel*>(m_session.m_idtable[an["channel"].as<int>()]);
							if(!channel)	//don't crash on bad IDs or missing filters
								break;
							size_t stream = 0;
							if(an["stream"])
								stream = an["stream"].as<int>();
							auto area = make_shared<WaveformArea>(StreamDescriptor(channel, stream), group, this);
							group->AddArea(area);

							//Add any overlays
							auto overlays = an["overlays"];
							for(auto jt : overlays)
							{
								auto filter = static_cast<Filter*>(m_session.m_idtable[jt.second["id"].as<int>()]);
								stream = 0;
								if(jt.second["stream"])
									stream = jt.second["stream"].as<int>();
								if(filter)
									area->AddStream(StreamDescriptor(filter, stream));
							}

							//FIXME: This borks on some v1 files that are mislabeled as v0
							//For now, ignore persistence settings on all v0/v1 files
							/*
							if (version == 0)
								area->SetPersistenceEnabled(an["persistence"].as<int>() == 1);
							else
								area->SetPersistenceEnabled(an["persistence"].as<bool>());
							*/

							break;
						}
					}
				}

				//post refactor, area nodes are named
				else
				{
					auto an = areas[string("area") + to_string(aid)];

					auto channel = static_cast<OscilloscopeChannel*>(m_session.m_idtable[an["channel"].as<int>()]);
					if(!channel)	//don't crash on bad IDs or missing filters
						continue;
					size_t stream = 0;
					if(an["stream"])
						stream = an["stream"].as<int>();
					auto area = make_shared<WaveformArea>(StreamDescriptor(channel, stream), group, this);
					group->AddArea(area);

					//Add any overlays
					auto overlays = an["overlays"];
					for(auto jt : overlays)
					{
						auto filter = static_cast<Filter*>(m_session.m_idtable[jt.second["id"].as<int>()]);
						stream = 0;
						if(jt.second["stream"])
							stream = jt.second["stream"].as<int>();
						if(filter)
							area->AddStream(StreamDescriptor(filter, stream));
					}

					//area->SetPersistenceEnabled(an["persistence"].as<bool>());
				}
			}

			//ngscopeclient has a single list of streams
			else
			{
				auto an = areas[string("area") + to_string(aid)];

				shared_ptr<WaveformArea> area;

				auto streams = an["streams"];
				for(auto jt : streams)
				{
					auto chan = static_cast<OscilloscopeChannel*>(m_session.m_idtable[jt.second["channel"].as<int>()]);
					auto stream = jt.second["stream"].as<int>();
					auto persist = jt.second["persistence"].as<bool>();
					auto ramp = jt.second["colorRamp"].as<string>();
					if(chan)
					{
						//Make the waveform area if needed
						if(!area)
						{
							area = make_shared<WaveformArea>(StreamDescriptor(chan, stream), group, this);
							area->RemoveStream(0);
						}

						area->AddStream(StreamDescriptor(chan, stream), persist, ramp);
					}
					else
					{
						LogWarning("channel %d not found in area %d\n",
							jt.second["channel"].as<int>(),
							aid);
					}
				}

				if(!area)
					LogWarning("no waveform area created for area %d\n", aid);
				else
					group->AddArea(area);
			}
		}
	}

	//Markers
	auto markers = node["markers"];
	if(markers)
	{
		for(auto it : markers)
		{
			auto inode = it.second;
			TimePoint timestamp(inode["timestamp"].as<int64_t>(), inode["time_fsec"].as<int64_t>());
			for(auto jt : inode["markers"])
				m_session.AddMarker(Marker(timestamp, jt.second["offset"].as<int64_t>(), jt.second["name"].as<string>()));
		}
	}

	//ignore splitter configuration from legacy format as imgui now handles that

	auto dialogs = node["dialogs"];
	if(dialogs)
	{
		if(!LoadDialogs(dialogs))
			return false;
	}

	//Measurements
	auto measurements = node["measurements"];
	if(measurements)
	{
		//Make the measurements dialog
		if(!m_measurementsDialog)
		{
			m_measurementsDialog = make_shared<MeasurementsDialog>(m_session);
			AddDialog(m_measurementsDialog);
		}

		int index;
		int stream;
		for(auto m : measurements)
		{
			auto sin = m.as<string>();
			if(2 != sscanf(sin.c_str(), "%d/%d", &index, &stream))
			{
				index = atoi(sin.c_str());
				stream = 0;
			}

			m_measurementsDialog->AddStream(
				StreamDescriptor(static_cast<OscilloscopeChannel*>(m_session.m_idtable[index]), stream));
		}
	}

	LogTrace("ui config loaded\n");
	return true;
}

/**
	@brief Load dialog configuration
 */
bool MainWindow::LoadDialogs(const YAML::Node& node)
{
	//TODO: all of the other dialog types

	auto analyzers = node["analyzers"];
	if(analyzers)
	{
		for(auto it : analyzers)
		{
			auto pd = static_cast<PacketDecoder*>(m_session.m_idtable[it.second.as<int>()]);

			auto dlg = make_shared<ProtocolAnalyzerDialog>(pd, m_session.GetPacketManager(pd), m_session, *this);
			m_protocolAnalyzerDialogs[pd] = dlg;
			AddDialog(dlg);
		}
	}

	auto meters = node["meters"];
	if(meters)
	{
		for(auto it : meters)
		{
			auto meter = dynamic_cast<SCPIMultimeter*>(
				static_cast<Instrument*>(m_session.m_idtable[it.second.as<int>()]));
			if(meter)
				m_session.AddMultimeterDialog(meter);
			else
			{
				ShowErrorPopup("Invalid meter", "Multimeter could not be loaded");
				return false;
			}
		}
	}

	auto generators = node["generators"];
	if(generators)
	{
		for(auto it : generators)
		{
			auto gen = dynamic_cast<SCPIFunctionGenerator*>(
				static_cast<Instrument*>(m_session.m_idtable[it.second.as<int>()]));

			if(gen)
				AddDialog(make_shared<FunctionGeneratorDialog>(gen, &m_session));
			else
			{
				ShowErrorPopup("Invalid function generator", "Function generator could not be loaded");
				return false;
			}
		}
	}

	auto berts = node["berts"];
	if(berts)
	{
		for(auto it : berts)
		{
			auto bert = dynamic_cast<SCPIBERT*>(
				static_cast<Instrument*>(m_session.m_idtable[it.second.as<int>()]));

			if(bert)
				AddDialog(make_shared<BERTDialog>(bert, m_session.GetBERTState(bert), &m_session));
			else
			{
				ShowErrorPopup("Invalid BERT", "BERT could not be loaded");
				return false;
			}
		}
	}

	auto loads = node["loads"];
	if(loads)
	{
		for(auto it : loads)
		{
			auto load = dynamic_cast<SCPILoad*>(
				static_cast<Instrument*>(m_session.m_idtable[it.second.as<int>()]));

			if(load)
				AddDialog(make_shared<LoadDialog>(load, m_session.GetLoadState(load), &m_session));
			else
			{
				ShowErrorPopup("Invalid load", "Load could not be loaded");
				return false;
			}
		}
	}

	//Single-instance dialogs

	auto log = node["logviewer"];
	if(log && log.as<bool>())
	{
		m_logViewerDialog = make_shared<LogViewerDialog>(this);
		AddDialog(m_logViewerDialog);
	}

	auto manageinst = node["manageinst"];
	if(manageinst && manageinst.as<bool>())
	{
		m_manageInstrumentsDialog = make_shared<ManageInstrumentsDialog>(m_session, this);
		AddDialog(m_manageInstrumentsDialog);
	}

	auto metrics = node["metrics"];
	if(metrics && metrics.as<bool>())
	{
		m_metricsDialog = make_shared<MetricsDialog>(&m_session);
		AddDialog(m_metricsDialog);
	}

	auto pref = node["preferences"];
	if(pref && pref.as<bool>())
	{
		m_preferenceDialog = make_shared<PreferenceDialog>(m_session.GetPreferences());
		AddDialog(m_preferenceDialog);
	}

	auto hist = node["history"];
	if(hist && hist.as<bool>())
	{
		m_historyDialog = make_shared<HistoryDialog>(m_session.GetHistory(), m_session, *this);
		AddDialog(m_historyDialog);
	}

	auto time = node["timebase"];
	if(time && time.as<bool>())
		ShowTimebaseProperties();

	auto trig = node["trigger"];
	if(trig && trig.as<bool>())
		ShowTriggerProperties();

	auto persist = node["persistence"];
	if(persist && persist.as<bool>())
	{
		m_persistenceDialog = make_shared<PersistenceSettingsDialog>(*this);
		AddDialog(m_persistenceDialog);
	}

	auto graph = node["filtergraph"];
	if(graph && graph.as<bool>())
	{
		m_graphEditor = make_shared<FilterGraphEditor>(m_session, this);
		AddDialog(m_graphEditor);
	}

	auto measure = node["measurements"];
	if(measure && measure.as<bool>())
	{
		m_measurementsDialog = make_shared<MeasurementsDialog>(m_session);
		AddDialog(m_measurementsDialog);
	}

	return true;
}

/**
	@brief Actually save a file (may be triggered by file|save or file|save as)
 */
void MainWindow::DoSaveFile(const string& sessionPath)
{
	//Stop the trigger so we don't have data races if a waveform comes in mid-save
	m_session.StopTrigger();

	//Saving the file conflicts with all other waveform data operations
	lock_guard<shared_mutex> lock(m_session.GetWaveformDataMutex());

	//Get the data directory for the session
	string base = sessionPath.substr(0, sessionPath.length() - strlen(".scopesession"));
	string datadir = base + "_data";
	LogDebug("Saving session file \"%s\" (data directory %s)\n", sessionPath.c_str(), datadir.c_str());

	//Serialize the session
	YAML::Node node{};
	if(!SaveSessionToYaml(node, datadir))
		return;

	//Write the generated YAML to disk
	ofstream outfs(sessionPath);
	if(!outfs)
	{
		ShowErrorPopup(
			"Cannot open file",
			string("Failed to open output session file \"") + sessionPath + "\" for writing");
		return;
	}

	outfs << node;
	outfs.close();

	if(!outfs)
	{
		ShowErrorPopup(
			"Write failed",
			string("Failed to write session file \"") + sessionPath + "\"");
	}

	//Add to recent files list
	m_sessionFileName = sessionPath;
	m_sessionDataDir = datadir;
	m_recentFiles[sessionPath] = time(nullptr);
	SaveRecentFileList();
}

/**
	@brief Serialize the current session to a YAML::Node

	@param node		Node for the main .scopesession
	@param dataDir	Path to the _data directory (may not have been created yet)

	@return			True if successful, false on error
 */
bool MainWindow::SaveSessionToYaml(YAML::Node& node, const string& dataDir)
{
	if(!SetupDataDirectory(dataDir))
		return false;

	/*
		version unspecified (treated as version 0): original string concatenation based glscopeclient impl
		version 1: yaml-cpp glscopeclient
		version 2: initial ngscopeclient
	 */
	node["version"] = 2;

	//Save the session state
	node["metadata"]  = m_session.SerializeMetadata();
	node["instruments"] = m_session.SerializeInstrumentConfiguration();
	node["triggergroups"]  = m_session.SerializeTriggerGroups();
	if(!Filter::GetAllInstances().empty())
		node["decodes"] = m_session.SerializeFilterConfiguration();

	//Save UI widgets
	node["ui_config"] = SerializeUIConfiguration();

	//TODO: waveform data
	if(!m_session.SerializeWaveforms(dataDir))
		return false;

	//Save ImGui configuration
	string ipath = dataDir + "/imgui.ini";
	ImGui::SaveIniSettingsToDisk(ipath.c_str());

	//Save imgui_node_editor settings
	ofstream outfs(dataDir + "/filtergraph.json");
	if(!outfs)
	{
		ShowErrorPopup(
			"Failed to save filter graph configuration",
			"Unable to open filtergraph.json for writing");
		return false;
	}
	outfs << m_graphEditorConfigBlob;
	outfs.close();

	return true;
}

/**
	@brief Make sure the data directory exists
 */
bool MainWindow::SetupDataDirectory(const string& dataDir)
{
	//See if the directory exists
	bool dir_exists = false;

#ifndef _WIN32
	int hfile = open(dataDir.c_str(), O_RDONLY);
	if(hfile >= 0)
	{
		//It exists as a file. Reopen and check if it's a directory
		::close(hfile);
		hfile = open(dataDir.c_str(), O_RDONLY | O_DIRECTORY);

		//If this open works, it's a directory.
		if(hfile >= 0)
		{
			::close(hfile);
			dir_exists = true;
		}

		//Data dir exists, but it's something else! Error out
		else
		{
			ShowErrorPopup(
				"Cannot save session",
				string("The requested data directory ") + dataDir + " already exists, but is not a directory!");
			return false;
		}
	}
#else
	auto fileType = GetFileAttributes(dataDir.c_str());

	// Check if any file exists at this path
	if(fileType != INVALID_FILE_ATTRIBUTES)
	{
		if(fileType & FILE_ATTRIBUTE_DIRECTORY)
		{
			// directory exists
			dir_exists = true;
		}
		else
		{
			// Its some other file
			ShowErrorPopup(
				"Cannot save session",
				string("The requested data directory ") + dataDir + " already exists, but is not a directory!");
			return false;
		}
	}
#endif

	//Create the directory we're saving to (if needed)
	if(!dir_exists)
	{
#ifdef _WIN32
		auto result = mkdir(dataDir.c_str());
#else
		auto result = mkdir(dataDir.c_str(), 0755);
#endif

		if(0 != result)
		{
			ShowErrorPopup(
				"Failed to save session",
				string("The data directory ") + dataDir + " could not be created!");
			return false;
		}
	}

	//Remove any existing waveform data
	char cwd[PATH_MAX];
	getcwd(cwd, PATH_MAX);

	chdir(dataDir.c_str());
	const auto directories = ::Glob("scope_*", true);
	for(const auto& directory: directories)
		::RemoveDirectory(directory);

	chdir(cwd);

	return true;
}

/**
	@brief Serialize waveform areas etc to a YAML::Node
 */
YAML::Node MainWindow::SerializeUIConfiguration()
{
	YAML::Node node;

	//Write new version 2 "window" section with size, since imgui does *not* save the size of the top level window
	YAML::Node window;
	window["height"] = m_height;
	window["width"] = m_width;
	window["fullscreen"] = m_fullscreen;
	node["window"] = window;

	//Waveform areas are hierarchical internally, but written as separate area and group headings
	YAML::Node areas;
	YAML::Node groups;
	for(auto group : m_waveformGroups)
	{
		int gid = m_session.m_idtable.emplace(group.get());

		auto wareas = group->GetWaveformAreas();
		for(auto area : wareas)
		{
			YAML::Node areaNode;
			int id = m_session.m_idtable.emplace(area.get());
			areaNode["id"] = id;

			//Legacy glscopeclient format had one "channel" and zero or more "overlays".
			//Now we just have a single "streams" array
			YAML::Node streamNode;
			for(size_t i=0; i<area->GetStreamCount(); i++)
				streamNode["stream" + to_string(i)] = area->GetDisplayedChannel(i)->Serialize(m_session.m_idtable);

			areaNode["streams"] = streamNode;
			areas["area" + to_string(id)] = areaNode;
		}

		//Add the group once we've put everything in it
		//(need IDs defined for all of the areas inside said group)
		groups["group" + to_string(gid)] = group->SerializeConfiguration(m_session.m_idtable);
	}

	node["areas"] = areas;
	node["groups"] = groups;

	//don't write legacy "splitters" section

	node["markers"] = m_session.SerializeMarkers();

	//Serialize dialogs
	node["dialogs"] = SerializeDialogs();

	//Serialize measurements
	if(m_measurementsDialog)
	{
		auto measurements = m_measurementsDialog->GetStreams();

		YAML::Node mnode;
		for(auto stream : measurements)
			mnode.push_back(to_string(m_session.m_idtable.emplace(stream.m_channel)) + "/" + to_string(stream.m_stream));

		node["measurements"] = mnode;
	}

	return node;
}

/**
	@brief Serializes the list of open dialogs
 */
YAML::Node MainWindow::SerializeDialogs()
{
	YAML::Node node;

	//Meter dialogs
	if(!m_meterDialogs.empty())
	{
		YAML::Node mnode;

		for(auto it : m_meterDialogs)
		{
			auto meter = it.first;
			mnode[meter->m_nickname] = m_session.m_idtable.emplace((Instrument*)meter);
		}

		node["meters"] = mnode;
	}

	if(!m_psuDialogs.empty())
	{
		YAML::Node gnode;

		for(auto it : m_psuDialogs)
		{
			auto psu = it.first;
			gnode[psu->m_nickname] = m_session.m_idtable.emplace((Instrument*)psu);
		}
		node["psus"] = gnode;
	}

	if(!m_loadDialogs.empty())
	{
		YAML::Node gnode;

		for(auto it : m_loadDialogs)
		{
			auto load = it.first;
			gnode[load->m_nickname] = m_session.m_idtable.emplace((Instrument*)load);
		}
		node["loads"] = gnode;
	}

	if(!m_generatorDialogs.empty())
	{
		YAML::Node gnode;

		for(auto it : m_generatorDialogs)
		{
			auto gen = it.first;
			gnode[gen->m_nickname] = m_session.m_idtable.emplace((Instrument*)gen);
		}
		node["generators"] = gnode;
	}

	if(!m_bertDialogs.empty())
	{
		YAML::Node gnode;

		for(auto it : m_bertDialogs)
		{
			auto bert = it.first;
			gnode[bert->m_nickname] = m_session.m_idtable.emplace((Instrument*)bert);
		}
		node["berts"] = gnode;
	}


	//TODO: rf generator dialogs
	//TODO: SCPI console
	//TODO: Channel properties

	//Protocol analyzers
	if(!m_protocolAnalyzerDialogs.empty())
	{
		YAML::Node anode;

		for(auto it : m_protocolAnalyzerDialogs)
		{
			auto proto = it.first;
			anode[proto->GetDisplayName()] = m_session.m_idtable.emplace(proto);
		}

		node["analyzers"] = anode;
	}


	/*

	///@brief Map of RF generators to generator control dialogs
	std::map<SCPIRFSignalGenerator*, std::shared_ptr<Dialog> > m_rfgeneratorDialogs;

	///@brief Map of instruments to SCPI console dialogs
	std::map<SCPIInstrument*, std::shared_ptr<Dialog> > m_scpiConsoleDialogs;

	///@brief Map of channels to properties dialogs
	std::map<OscilloscopeChannel*, std::shared_ptr<Dialog> > m_channelPropertiesDialogs;

	*/

	//Manage instruments dialog has no separate settings
	if(m_manageInstrumentsDialog)
		node["manageinst"] = true;

	//Logfile viewer has no separate settings
	if(m_logViewerDialog)
		node["logviewer"] = true;

	//Metrics dialog has no separate settings
	if(m_metricsDialog)
		node["metrics"] = true;

	//Preferences dialog has no separate settings
	if(m_preferenceDialog)
		node["preferences"] = true;

	//History
	if(m_historyDialog)
		node["history"] = true;

	//Timebase
	if(m_timebaseDialog)
		node["timebase"] = true;

	//Trigger
	if(m_triggerDialog)
		node["trigger"] = true;

	//Persistence settings
	if(m_persistenceDialog)
		node["persistence"] = true;

	//Graph editor
	if(m_graphEditor)
		node["filtergraph"] = true;

	//Measurements
	if(m_measurementsDialog)
		node["measurements"] = true;

	return node;
}

void MainWindow::SaveRecentFileList()
{
	//Make a reverse mapping
	std::map<time_t, vector<string> > reverseMap;
	for(auto it : m_recentFiles)
		reverseMap[it.second].push_back(it.first);

	//Deduplicate timestamps
	set<time_t> timestampsDeduplicated;
	for(auto it : m_recentFiles)
		timestampsDeduplicated.emplace(it.second);

	//Sort the list by most recent
	vector<time_t> timestamps;
	for(auto t : timestampsDeduplicated)
		timestamps.push_back(t);
	std::sort(timestamps.rbegin(), timestamps.rend());

	//Add new ones
	int nleft = m_session.GetPreferences().GetInt("Files.max_recent_files");

	//Generate the output data
	YAML::Node node{};
	int j = 0;
	for(auto t : timestamps)
	{
		auto paths = reverseMap[t];
		for(auto fpath : paths)
		{
			YAML::Node child;
			child["path"] = fpath;
			child["timestamp"] = t;

			node[string("file") + to_string(j)] = child;
			j++;
		}

		nleft --;
		if(nleft == 0)
			break;
	}

	//Save to file
	auto fname = m_session.GetPreferences().GetConfigDirectory() + "/recentfiles.yml";
	ofstream outfs(fname);
	if(!outfs)
	{
		ShowErrorPopup(
			"Cannot open file",
			string("Failed to open recent-files file \"") + fname + "\" for writing");
		return;
	}
	outfs << node;
	outfs.close();
}

void MainWindow::LoadRecentFileList()
{
	try
	{
		auto docs = YAML::LoadAllFromFile(m_session.GetPreferences().GetConfigDirectory() + "/recentfiles.yml");
		if(docs.empty())
			return;
		auto node = docs[0];

		for(auto it : node)
		{
			auto inst = it.second;
			m_recentFiles[inst["path"].as<string>()] = inst["timestamp"].as<long long>();
		}
	}
	catch(const YAML::BadFile& ex)
	{
		LogDebug("Unable to open recently used files list (bad file)\n");
		return;
	}
	catch(const YAML::ParserException& ex)
	{
		LogDebug("Unable to open recently used files list (parser exception)\n");
		return;
	}

}

