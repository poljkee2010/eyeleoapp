#include "main.h"
#include "wx/tipwin.h"
#include "wx/taskbar.h"
#include "wx/sizer.h"
#include "wx/button.h"
#include "wx/checkbox.h"
#include "wx/combobox.h"
#include "wx/stattext.h"
#include "pugixml.hpp"
#include "bigpause_wnd.h"
#include "beforepause_wnd.h"
#include "minipause_wnd.h"
#include "notification_wnd.h"
#include "waiting_wnd.h"
#include "oscapabilities.h"
#include <algorithm>
#include "timeloc.h"
#include "activity_monitor.h"
#include "settings_wnd.h"
#include "eyecoach_resources.h"
#include "language_set.h"
#include "debug_wnd.h"
#include "excercises.h"
#include "DataLog.h"
#include "settings.h"

#ifdef WIN32
	#include <shellapi.h>
	#include "shlobj.h"
	#include <direct.h>
#endif

static EyeApp * g_eyeApp = 0;

IMPLEMENT_APP(EyeApp);

///////////////////////////////////////////////////////////////////////////////////////

EyeApp * getApp()
{
	return g_eyeApp;
}

///////////////////////////////////////////////////////////////////////////////////////
//wxDEFINE_EVENT(EXECUTE_TASK_EVENT, wxCommandEvent);

BEGIN_EVENT_TABLE(EyeApp, wxApp)
	EVT_QUERY_END_SESSION(EyeApp::OnQueryEndSession)
	EVT_END_SESSION(EyeApp::OnEndSession)
	EVT_COMMAND(wxID_ANY, EXECUTE_TASK_EVENT, EyeApp::OnTaskEvent)
END_EVENT_TABLE()


EyeApp::EyeApp() : 
	_settingsWnd(0),
	_inactivityTime(0),
	_timeLeftToBigPause(0),
	_timeLeftToMiniPause(0),
	_relaxingTimeLeft(0),
	_fullscreenBlockDuration(0),
	_postponeCount(0),
	_warningInterval(0),
	_debugWindow(0),
	_firstLaunch(true),
	_seenSettingsWindow(false),
	_fastMode(false),
	_userLongBreakCount(0),
	_userEarlySkipCount(0),
	_userLateSkipCount(0),
	_userRefuseCount(0),
	_userPostponeCount(0),
	_userAutoBreakCount(0),
	_userShortBreakCount(0),
	_lastBigPauseTimeLeft(0),
	_lastMiniPauseTimeLeft(0),
	_lastDuration(0),
	_currentState(0),
	_nextState(0),
	_finished(false),
	_lastShutdown(),
	_notificationWnd(nullptr),
	_showedLongBreakCountdown(false)
{
	g_eyeApp = this;
}

void EyeApp::ReadConfig()
{
	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file("config.xml");
	
	if (result.status == pugi::status_ok)
	{
		pugi::xml_node node = doc.child(L"config");
		if (!node.empty())
		{
			_lang.assign(node.attribute(L"language").value());
			_version.assign(node.attribute(L"version").value());
			_website.assign(node.attribute(L"website").value());
		}
	}
}

bool EyeApp::OnInit()
{
	if (!IsOnlyInstance())
		return false;

	InitializeDataLog();
	
	srand((unsigned)time(0));
	
	fillOSCapabilities();

	_lang = L"en";
	_version = L"(?)";
	_website = L"eyeleo.com";
	ReadConfig();

	if (!LoadLanguagePack(_lang))
	{
		wxMessageBox(_("Can't load '")+ _lang +_("' language pack."), _("EyeLeo"));
		if (!LoadLanguagePack(L"en"))
			return false;
	}
	
	wxInitAllImageHandlers();
	
	LoadEyeLeoResources();
	
	_taskBarIcon = new EyeTaskBarIcon();

	g_TaskMgr = new TaskManager();
	
	wxThreadError err = g_TaskMgr->Run();
	if (err != wxTHREAD_NO_ERROR)
	{
		wxMessageBox(_("Can't start timer thread!"));
		return false;
	}
	
#ifndef MASTER_RELEASE
	//_fastMode = true;
#endif
	
	ResetSettings();

	if (!LoadSettings())
		ResetSettings();
	else
		CheckSettings();
	
	if (_firstLaunch)
	{
		ChangeState(STATE_FIRST_LAUNCH, 1000);
	}
	else
	{
		if (_lastShutdown.IsValid())
		{
			wxDateTime now = wxDateTime::Now();
			
			wxTimeSpan lastShutdownP = now.Subtract(_lastShutdown);
			if (lastShutdownP.GetMinutes() <= 30)
			{
				wxLongLong_t p = lastShutdownP.GetMilliseconds().GetValue();
				long lastBigPauseTimeLeft = _lastBigPauseTimeLeft - p;
				long lastMiniPauseTimeLeft = _lastMiniPauseTimeLeft - p;
				
				if (lastBigPauseTimeLeft > 0)
				{
					if (lastBigPauseTimeLeft < 1000 * 60)
						lastBigPauseTimeLeft = 1000 * 60;

					SetBigPauseTime(lastBigPauseTimeLeft);
					
					if (lastMiniPauseTimeLeft < 1000 * 60)
						lastMiniPauseTimeLeft = 1000 * 60;

					if (lastMiniPauseTimeLeft > 0)
						SetMiniPauseTime(lastMiniPauseTimeLeft);
					else
						RestartMiniPauseInterval();
				}
				else
				{
					RestartBigPauseInterval();
					RestartMiniPauseInterval();
				}

				UpdateTaskbarText();
			}
			else
			{
				// normal case
				ApplySettings();
			}
		}
		else
		{
			ApplySettings();
		}
	}
	
	PrepareActivityMonitor();
	InstallActivityMonitor();
	
	g_Personage = new PersonageData(L"leopard");
	
	//_debugWindow = new DebugWindow();
	//_debugWindow->Show(true);

	int big_pause_seconds = _timeLeftToBigPause / 1000;
	wxString text = wxString::Format(langPack->get("tb_notification_first_launch"), getTimeStr(big_pause_seconds, SECONDS, _lang));
	_taskBarIcon->ShowBalloon(langPack->get("tb_popup_default"), text, 1000 * 40, wxICON_INFORMATION);
	
	return true;
}

bool EyeApp::IsOnlyInstance() const
{
#ifdef NDEBUG
	HANDLE hMutex = CreateMutexA(NULL, TRUE, "EyeLeo_mutex");
#else
	HANDLE hMutex = CreateMutexA(NULL, TRUE, "EyeLeo_mutex_d");
#endif
	if (GetLastError())
	{
		ReleaseMutex(hMutex);
		return false;
	}
	return true;
}

wxString EyeApp::GetSavePath() const
{
	static bool isInited = false;
	static wxString savePath;

#ifdef WIN32
	if (!isInited)
	{
		wchar_t appDataPath[MAX_PATH];
		
		// Getting a special path
		// CSIDL_COMMON_APPDATA -> 'C:\Documents and Settings\All Users\Application Data\'
		// CSIDL_APPDATA -> 'C:\Documents and Settings\username\Application Data\'
		// CSIDL_COMMON_DOCUMENTS -> 'C:\Documents and Settings\All Users\Documents\'
		if (SHGetSpecialFolderPathW(NULL, appDataPath, CSIDL_APPDATA, TRUE) == FALSE)
		{
			assert(!"SHGetSpecialFolderPath failed");
			return wxString(L"");
		}
		
		savePath.assign(appDataPath);
		savePath += L"\\EyeLeo\\";
		_wmkdir(savePath.c_str());
		
		isInited = true;
	}
#endif

	return savePath;
}

void EyeApp::RestartBigPauseInterval()
{
	LogMsg("RestartBigPauseInterval");

	_postponeCount = 0;
	_inactivityTime = 0;
	_showedLongBreakCountdown = false;
	
	if (_enableBigPause)
		_timeLeftToBigPause = _bigPauseInterval * 1000 * 60;
	else
		_timeLeftToBigPause = 0;
	
	ChangeState(STATE_IDLE, 1000);
	
	UpdateDebugWindow();
}

void EyeApp::SetBigPauseTime(long ms)
{
	LogMsg(wxString::Format(L"SetBigPauseTime to %d", ms));
	
	_postponeCount = 0;
	_inactivityTime = 0;
	
	if (ms < 1000 * 91) // not less than 1.5mins
	{
		ms = 1000 * 91;
	}
	else if (ms > 1000 * 60 * _bigPauseDuration) // not more current duration setting (bug check)
	{
		LogMsg(wxString::Format(L"Error! SetBigPauseTime"));
		ms = 1000 * 60 * _bigPauseDuration;
	}

	if (_enableBigPause)
		_timeLeftToBigPause = ms;
	else
		_timeLeftToBigPause = 0;
	
	ChangeState(STATE_IDLE, 1000);
	
	UpdateDebugWindow();
}

void EyeApp::RestartMiniPauseInterval()
{
	LogMsg("RestartMiniPauseInterval");

	if (_enableMiniPause)
		_timeLeftToMiniPause = _miniPauseInterval * 1000 * 60;
	else
		_timeLeftToMiniPause = 0;
	
	UpdateDebugWindow();
}

void EyeApp::SetMiniPauseTime(long ms)
{
	LogMsg(wxString::Format(L"SetMiniPauseTime to %d", ms));

	if (ms < 1000 * 91)
		ms = 1000 * 91;

	if (_enableMiniPause)
		_timeLeftToMiniPause = ms;
	else
		_timeLeftToMiniPause = 0;
	
	UpdateDebugWindow();
}

void EyeApp::OnUserActivity()
{
	if (GetNextState() == STATE_SUSPENDED)
		return;

	_inactivityTime = 0;
	
	UpdateDebugWindow();
	
	if (GetNextState() == STATE_AUTO_RELAX)
	{
		LogMsg("OnUserActivity ended auto-relax");
		ApplySettings();

		int big_pause_seconds = _timeLeftToBigPause / 1000;
		wxString text = wxString::Format(langPack->get("tb_notification_auto_relax_ended"), getTimeStr(big_pause_seconds, SECONDS, _lang));
		_taskBarIcon->ShowBalloon(langPack->get("tb_popup_default"), text, 1000 * 8, wxICON_INFORMATION);
	}
}

// Check if full screen app is running, returns display number or -1 as 'display'
bool EyeApp::IsFullscreenAppRunning(int * display, HWND * fullscreenWndHandle) const
{
	//wxLogDebug("IsFullscreenAppRunning");
	
    HWND hWnd = GetForegroundWindow();
	if (!hWnd)
		return false;

	HWND hDesktop = GetDesktopWindow();
	HWND hShell = GetShellWindow();
	if (hWnd == hDesktop || hWnd == hShell)
		return false;

    if (!IsWindowVisible(hWnd) || IsIconic(hWnd))
        return false;
	
	RECT wndArea;
    if (!GetWindowRect(hWnd, &wndArea))
        return false;

	refillResolutionParams();

	//wxLogDebug("wndArea (%d, %d), (%d, %d)", wndArea.left, wndArea.right, wndArea.top, wndArea.bottom);

	for (int d = 0; d < osCaps.numDisplays; ++d)
	{
		wxRect displayArea = osCaps.displays[d].geometry;
		
		//wxLogDebug("%d) displayArea (%d, %d, %d, %d)", d, displayArea.x, displayArea.y, displayArea.width, displayArea.height);
		
		if (wndArea.left == displayArea.x &&
			wndArea.top == displayArea.y &&
			wndArea.right - wndArea.left == displayArea.width &&
			wndArea.bottom - wndArea.top == displayArea.height)
		{
			if (display)
				*display = d;
			if (fullscreenWndHandle)
				*fullscreenWndHandle = hWnd;
			return true;
		}
	}
	return false;
}

void EyeApp::UpdateTaskbarText()
{
	if (GetNextState() == STATE_AUTO_RELAX)
	{
		_taskBarIcon->UpdateTooltip(L"EyeLeo error! Please contact author and tell him you saw this...");
		return;
	}
	
	if (GetNextState() == STATE_SUSPENDED)
	{
		int secs = _inactivityTime / 1000;
		//secs = (secs <= 50) ? secs : (secs + 59);
		wxString text = wxString::Format(langPack->get("tb_popup_paused"), getTimeStr(secs, SECONDS, _lang));
		_taskBarIcon->UpdateTooltip(text);
		return;
	}
	
	if (_enableBigPause)
	{
		int secs = _timeLeftToBigPause / 1000;
		//secs = (secs <= 50) ? secs : (secs + 59);
		wxString text = wxString::Format(langPack->get("tb_popup_active_1"), getTimeStr(secs, SECONDS, _lang));
		_taskBarIcon->UpdateTooltip(text);
	}
	else
	{
		_taskBarIcon->UpdateTooltip(langPack->get("tb_popup_default"));
	}
}

void EyeApp::ChangeState(int nextState, int duration)
{
	_lastDuration = duration;
	_nextState = nextState;
	
	g_TaskMgr->addTask("EyeApp", duration);
}

void EyeApp::RepeatState()
{
	ChangeState(_currentState, _lastDuration);
}

void EyeApp::Stop()
{
	if (g_TaskMgr)
	{
		g_TaskMgr->stop();
		g_TaskMgr->Delete();
	}
	g_TaskMgr = 0;
}

void EyeApp::OnTaskEvent(wxCommandEvent &c)
{
	TaskPayload * pl = static_cast<TaskPayload *>(c.GetClientData());
	if (!pl)
		return;

	wxMilliClock_t now = ::wxGetLocalTimeMillis();
	wxMilliClock_t time_went = now - pl->start_time;
	float f = float(time_went.ToDouble() / pl->duration.ToDouble());

	if (pl->check != 12345)
	{
		LogMsg(wxString::Format("Detected wrong event! pl=%x", pl));
		assert(false);
	}

	TaskPtr task = getWindow(pl->address);
	if (task)
		task->executeTask(f, time_went.ToLong());
	
	delete pl;
}

bool EyeApp::CheckInactivity()
{
	POINT p;
	BOOL res = GetCursorPos(&p);
	if (!res)
		return false;
	
	if ( abs(_cursorPos.x - p.x) > 1 || abs(_cursorPos.y - p.y) > 1 )
	{
		_cursorPos = p;
		return false;
	}
	
	return true;
}

Task *EyeApp::getWindow(const std::string &address)
{
	if (address == "EyeApp")
		return this;

	for (std::vector<BigPauseWindow*>::iterator wnd = _bigPauseWnds.begin(); wnd != _bigPauseWnds.end(); ++wnd)
	{
		if ((*wnd)->getName() == address)
			return *wnd;
	}

	for (std::vector<MiniPauseWindow*>::iterator wnd = _miniPauseWnds.begin(); wnd != _miniPauseWnds.end(); ++wnd)
	{
		if ((*wnd)->getName() == address)
			return *wnd;
	}

	for (std::vector<WaitingFullscreenWindow*>::iterator wnd = _waitWnds.begin(); wnd != _waitWnds.end(); ++wnd)
	{
		if ((*wnd)->getName() == address)
			return *wnd;
	}

	if (_notificationWnd && _notificationWnd->getName() == address)
		return _notificationWnd;

	for (std::vector<BeforePauseWindow*>::iterator wnd = _beforePauseWnds.begin(); wnd != _beforePauseWnds.end(); ++wnd)
	{
		if ((*wnd)->getName() == address)
			return *wnd;
	}

	return nullptr;
}

void EyeApp::executeTask(float f, long time_went)
{
	(void)f;

	_currentState = _nextState;
	_nextState = 0;
	switch(_currentState)
	{
	case STATE_SUSPENDED:
		{
			RepeatState();
			
			// ��� ����� ��������� ���-��� _inactivityTime � �������� �������� (���� � ������� ������ �� �������������)
			int multiplier = _fastMode ? 2 : 1;
			_inactivityTime -= time_went * multiplier;
			if (_inactivityTime <= 0)
			{
				_taskBarIcon->ShowBalloon(langPack->get("tb_popup_default"), langPack->get("tb_notification_start_after_pause"), 1000 * 40, wxICON_INFORMATION);
				RestartBigPauseInterval();
				RestartMiniPauseInterval();

				SaveSettings();
			}

			UpdateTaskbarText();
		}
		break;

	case STATE_FIRST_LAUNCH:
		{
			wxString text = wxString::Format(langPack->get("tb_notification_first_launch"), getTimeStr(_bigPauseInterval, MINUTES, _lang));
			_taskBarIcon->ShowBalloon(langPack->get("tb_popup_default"), text, 1000 * 40, wxICON_INFORMATION);

			_firstLaunch = false;

			SaveSettings();
			ApplySettings();
		}
		break;
	
	case STATE_IDLE:
		if (_enableBigPause || _enableMiniPause)
		{
			//LogMsg(wxString::Format("idle: _timeLeftToBigPause=%d, _timeLeftToMiniPause=%d, _inactivityTime=%d", _timeLeftToBigPause, _timeLeftToMiniPause, _inactivityTime));
			
			RepeatState();
			
			UpdateTaskbarText();

			CheckSettings();
			
			if (CheckInactivity())
			{
				_inactivityTime += time_went * (_fastMode ? 1 : 1);
				if (_inactivityTime >= 5 * 60 * 1000) // 5 mins
				{
					AutoRelax();
				}
			}
			else
			{
				_inactivityTime = 0;
			}
			
			UpdateDebugWindow();
			
			if (_enableBigPause && _timeLeftToBigPause > 0)
			{
				int multiplier = _fastMode ? 5 : 1;
				_timeLeftToBigPause -= time_went * multiplier;
				
				UpdateDebugWindow();
				
				if (_warningInterval > 0.0f)
				{
					if (_timeLeftToBigPause <= _warningInterval * 60 * 1000 &&
						_timeLeftToBigPause > eyeleo::settings::timeForLongBreakConfirmation * 1000)
					{
						if (!NotificationWindow::hasAnyInstance() && !_showedLongBreakCountdown)
						{
							// ������� countdown ����, �� ������ �� ������ fullscreen ����������
							int fullscreenDisplay = -1;
							bool isFullscreen = IsFullscreenAppRunning(&fullscreenDisplay);

							for (int displayInd = 0; displayInd < osCaps.numDisplays; ++displayInd)
							{
								if (isFullscreen && fullscreenDisplay == displayInd)
									continue;

								_timeLeftToBigPause = _warningInterval * 60 * 1000;

								NotificationWindow * wnd = new NotificationWindow();
								wnd->Init();
								wnd->SetTime(_timeLeftToBigPause);
								wnd->Show(true);
								_notificationWnd = wnd;

								LogMsg(wxString("Notification window opened"));

								_showedLongBreakCountdown = true;
								//_fastMode = false;

								break;
							}
						}
					}
				}
				
				if (_timeLeftToBigPause <= eyeleo::settings::timeForLongBreakConfirmation * 1000)
				{
					ChangeState(STATE_START_BIG_PAUSE, 100);
				}
			}
			if (_enableMiniPause && _timeLeftToMiniPause > 0)
			{
				int multiplier = _fastMode ? 14 : 1;
				_timeLeftToMiniPause -= time_went * multiplier;
				
				UpdateDebugWindow();
				
				if (_timeLeftToBigPause == 0 || _timeLeftToBigPause > _miniPauseInterval * 1000 * 60 / 2) // don't show mini-pause if big pause is about to start
				{
					if (_timeLeftToMiniPause <= 0)
					{
						StartMiniPause();

						SaveSettings();
					}
				}
			}
		}
		else
		{
			ChangeState(STATE_SUSPENDED, 300);
		}
		break;
	
	case STATE_WAITING_SCREEN:
		{
			//LogMsg(wxString::Format("waiting: _fullscreenBlockDuration=%d", _fullscreenBlockDuration));
			RepeatState();
			
			int multiplier = _fastMode ? 1 : 1;
			_fullscreenBlockDuration += time_went * multiplier;
			
			if (_fullscreenBlockDuration >= 1000 * 60 * 5 && _fullscreenBlockDuration < 1000 * 60 * 6)
			{
				ShowWaitingWnd();
			}
			
			if (_fullscreenBlockDuration >= 1000 * 60 * 8) // after 8 mins
			{
				// cancel current big pause
				RestartBigPauseInterval();

				SaveSettings();
			}
			else
			{
				bool fullscreenBlock = IsFullscreenAppRunning();
				if (fullscreenBlock)
				{
					ChangeState(STATE_WAITING_SCREEN, 3000);
				}
				else
				{
					CloseWaitingWnd();
					ChangeState(STATE_START_BIG_PAUSE, 3000);
				}
			}
		}
		break;
	
	case STATE_START_BIG_PAUSE:
		{
			//LogMsg(wxString("start big pause:"));
			if (_enableStrictMode)
			{
				HWND hwnd;
				bool fullscreenBlock = IsFullscreenAppRunning(0, &hwnd);
				if (fullscreenBlock && hwnd)
				{
					ShowWindow(hwnd, SW_FORCEMINIMIZE);
					ChangeState(STATE_START_BIG_PAUSE, 2000);
				}
				else
				{
					StartBigPause();
				}
			}
			else
			{
				bool fullscreenBlock = IsFullscreenAppRunning();
				if (!fullscreenBlock)
				{
					AskForBigPause();
				}
				else
				{
					ShowWaitingWnd();
					_fullscreenBlockDuration = 0;
					ChangeState(STATE_WAITING_SCREEN, 1000);
				}
			}
		}
		break;
	
	case STATE_AUTO_RELAX:
		RepeatState();
		//LogMsg(wxString("auto relax:"));
		_inactivityTime += time_went;
		
		if (!CheckInactivity())
		{
			OnUserActivity();
		}
		//UninstallActivityMonitor();
		//InstallActivityMonitor();
		
		break;
	
	case STATE_RELAXING:
		{
			//LogMsg(wxString::Format("relaxing: _relaxingTimeLeft=%d", _relaxingTimeLeft));

			int multiplier = _fastMode ? 1 : 1;
			_relaxingTimeLeft -= time_went * multiplier;
			
			UpdateDebugWindow();
			
			if (_relaxingTimeLeft < 0)
			{
	#ifdef WIN32
				if (_enableSounds)
					::PlaySound(L"SystemExclamation", NULL, SND_ALIAS | SND_ASYNC);
	#endif
				StopBigPause();
				SaveSettings();
			}
			else
			{
				RepeatState();
			}
		}
		break;

	case STATE_DESTROY:
		{
			Exit();
		}
		break;
	}
}

void EyeApp::UpdateDebugWindow()
{
	if (!_debugWindow)
		return;
	_debugWindow->_timeLeftToBigPause->SetLabel(wxString::Format(L"%d", _timeLeftToBigPause));
	_debugWindow->_timeLeftToMiniPause->SetLabel(wxString::Format(L"%d", _timeLeftToMiniPause));
	_debugWindow->_inactivityTime->SetLabel(wxString::Format(L"%d", _inactivityTime));
	_debugWindow->_relaxingTimeLeft->SetLabel(wxString::Format(L"%d", _relaxingTimeLeft));
}

void EyeApp::AskForBigPause()
{
	BeforePauseWindow * wnd = new BeforePauseWindow(0, _postponeCount);
	wnd->Init();
	wnd->Show(true);

	_beforePauseWnds.push_back(wnd);
}

void EyeApp::PostponeBigPause()
{
	_showedLongBreakCountdown = false;
	_userPostponeCount++;
	_postponeCount++;
	_inactivityTime = 0;
	_timeLeftToBigPause = 3000 * 60; // 3 mins
	_timeLeftToMiniPause = 0;
	ChangeState(STATE_IDLE, 1000);
	
	UpdateDebugWindow();
	UpdateTaskbarText();
}

void EyeApp::RefuseBigPause()
{
	_userRefuseCount++;
	RestartBigPauseInterval();
	RestartMiniPauseInterval();

	SaveSettings();
}

void EyeApp::AutoRelax()
{
	LogMsg("AutoRelax");

	UninstallActivityMonitor();
	InstallActivityMonitor();

	_inactivityTime = 0;
	_timeLeftToBigPause = 0;
	_timeLeftToMiniPause = 0;
	_userAutoBreakCount++;
	ChangeState(STATE_AUTO_RELAX, 500);
	UpdateTaskbarText();
	
	UpdateDebugWindow();
}

void EyeApp::StartBigPause()
{
	if (!_bigPauseWnds.empty())
	{
		// we should only get here from Settings Wnd
		return;
	}
	
	LogMsg("StartBigPause");

	_showedLongBreakCountdown = false;
	
	bool fullscreenBlock = IsFullscreenAppRunning();
	if (!fullscreenBlock)
	{
		_userLongBreakCount++;
		
		StopMiniPause();
		
		assert(_bigPauseWnds.empty());
		for (int displayInd = 0; displayInd < osCaps.numDisplays; ++displayInd)
		{
			BigPauseWindow * wnd = new BigPauseWindow(displayInd);
			//LogMsg(wxString::Format("_bigPauseDuration = %d", _bigPauseDuration * 60));
			wnd->Init();
			wnd->SetBreakDuration(_bigPauseDuration * 60);
			wnd->Show(true);
			_bigPauseWnds.push_back(wnd);
		}
		_bigPauseWnds[0]->SetFocus();
		
		_relaxingTimeLeft = _bigPauseDuration * 1000 * 60;
		ChangeState(STATE_RELAXING, 1000);
		
		UpdateDebugWindow();
	}
	else
	{
		LogMsg("fullscreen block, show wait wnd");
		
		ShowWaitingWnd();
		_fullscreenBlockDuration = 0;
		ChangeState(STATE_WAITING_SCREEN, 1000);
	}
}

void EyeApp::ShowWaitingWnd()
{
	if (!_waitWnds.empty())
		return;
	
	int fullscreenDisplay = -1;
	IsFullscreenAppRunning(&fullscreenDisplay);
	
	for (int displayInd = 0; displayInd < osCaps.numDisplays; ++displayInd)
	{
		if (fullscreenDisplay == displayInd)
			continue;
		
		WaitingFullscreenWindow * wnd = new WaitingFullscreenWindow();
		wnd->Init(displayInd);
		wnd->Show();
		
		_waitWnds.push_back(wnd);
	}
}

void EyeApp::CloseWaitingWnd()
{
	if (!_waitWnds.empty())
	{
		for (std::vector<WaitingFullscreenWindow *>::iterator it = _waitWnds.begin(); it != _waitWnds.end(); ++it)
		{
			WaitingFullscreenWindow * wnd = (*it);
			wnd->Hide();
		}
		_waitWnds.resize(0);
	}
}

void EyeApp::OnCloseWaitingWnd(WaitingFullscreenWindow * ptr)
{
	std::vector<WaitingFullscreenWindow *>::iterator it = std::find(_waitWnds.begin(), _waitWnds.end(), ptr);
	if (it != _waitWnds.end())
	{
		_waitWnds.erase(it);
	}
}

void EyeApp::OnCloseBeforePauseWnd(BeforePauseWindow * ptr)
{
	std::vector<BeforePauseWindow *>::iterator it = std::find(_beforePauseWnds.begin(), _beforePauseWnds.end(), ptr);
	if (it != _beforePauseWnds.end())
	{
		_beforePauseWnds.erase(it);
	}
}

void EyeApp::StopBigPause()
{
	LogMsg(wxString::Format("EyeApp::StopBigPause, bigPauseWnds.size=%d", _bigPauseWnds.size()));
	if (!_bigPauseWnds.empty())
	{
		for (std::vector<BigPauseWindow *>::iterator it = _bigPauseWnds.begin(); it != _bigPauseWnds.end(); ++it)
		{
			BigPauseWindow * wnd = (*it);
			wnd->Hide();
		}
	}
	
	RestartBigPauseInterval();
	RestartMiniPauseInterval();

	SaveSettings();

	LogMsg(wxString::Format("EyeApp::StopBigPause end"));
}

void EyeApp::OnSkipBigPauseClicked()
{
	long fullPeriod = _bigPauseDuration * 1000 * 60;
	long earlyThreshold = long(float(fullPeriod) * 0.35f);

	if (_relaxingTimeLeft > earlyThreshold)
		_userEarlySkipCount++;
	else
		_userLateSkipCount++;

	StopBigPause();
}

void EyeApp::StartMiniPause()
{
	LogMsg("StartMiniPause");
	
	int fullscreenDisplay = -1;
	//bool fullscreenBlock = IsFullscreenAppRunning(&fullscreenDisplay);
	
	if (_miniPauseWnds.empty())
	{
		_userShortBreakCount++;

		for (int displayInd = 0; displayInd < osCaps.numDisplays; ++displayInd)
		{
			if (fullscreenDisplay == displayInd)
				continue;
			
			MiniPauseWindow * wnd = new MiniPauseWindow(displayInd, _userShortBreakCount);
			wnd->Init();
			wnd->Show(true);
			
			_miniPauseWnds.push_back(wnd);
		}
	}
	else
	{
		LogMsg("(!) _miniPauseWnds is not empty");
	}
	
	// play sound
	
	RestartMiniPauseInterval(); // little flaw
}

void EyeApp::StopMiniPause()
{
	if (!_miniPauseWnds.empty())
	{
		for (std::vector<MiniPauseWindow *>::iterator it = _miniPauseWnds.begin(); it != _miniPauseWnds.end(); ++it)
		{
			MiniPauseWindow * wnd = (*it);
			wnd->Hide();
		}
	}
	
	RestartMiniPauseInterval();
}

void EyeApp::OnMiniPauseWindowClosed(MiniPauseWindow * ptr)
{
	std::vector<MiniPauseWindow *>::iterator it = std::find(_miniPauseWnds.begin(), _miniPauseWnds.end(), ptr);
	assert(it != _miniPauseWnds.end());
	if (it != _miniPauseWnds.end())
		_miniPauseWnds.erase(it);
}

void EyeApp::OnBigPauseWindowClosed(BigPauseWindow *ptr)
{
	std::vector<BigPauseWindow *>::iterator it = std::find(_bigPauseWnds.begin(), _bigPauseWnds.end(), ptr);
	assert(it != _bigPauseWnds.end());
	if (it != _bigPauseWnds.end())
		_bigPauseWnds.erase(it);

	it = std::find(_bigPauseWnds.begin(), _bigPauseWnds.end(), ptr);
	assert(it == _bigPauseWnds.end());
}

void EyeApp::OnNotificationWindowClosed()
{
	_notificationWnd = nullptr;
}

void EyeApp::OpenSettings()
{
	if ( _settingsWnd )
	{
		::SetForegroundWindow(_settingsWnd->GetHWND());
		return;
	}
	
	wxString appendix;
#ifndef MASTER_RELEASE
	appendix = _("");
#endif
	SettingsWindow *wnd = new SettingsWindow(langPack->get("settings_title") + appendix);
	wnd->Show(true);
	_settingsWnd = wnd;

	_seenSettingsWindow = true;
}

bool EyeApp::LoadSettings()
{
	LogMsg("LoadSettings");

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(GetSavePath() + "settings.xml");
	
	if (result.status != pugi::status_ok)
		return false;

	pugi::xml_node nodeSettings = doc.child(L"settings");
	if (nodeSettings.empty())
		return false;
	
	for (pugi::xml_node node = nodeSettings.first_child(); node; node = node.next_sibling())
	{
		const wchar_t * name = node.name();
		if (wcscmp(name, L"statistics") == 0)
		{
			_firstLaunch = node.attribute(L"first_launch").as_bool();
			_seenSettingsWindow = node.attribute(L"seen_settings").as_bool();
			_userLongBreakCount = node.attribute(L"long_break_count").as_uint();
			_userEarlySkipCount = node.attribute(L"early_skip_count").as_uint();
			_userLateSkipCount = node.attribute(L"late_skip_count").as_uint();
			_userRefuseCount = node.attribute(L"refuse_count").as_uint();
			_userPostponeCount = node.attribute(L"postpone_count").as_uint();
			_userAutoBreakCount = node.attribute(L"auto_break_count").as_uint();
			_userShortBreakCount = node.attribute(L"short_break_count").as_uint();
			_lastShutdown.ParseISOCombined(node.attribute(L"last_shutdown").value());
			_lastBigPauseTimeLeft = node.attribute(L"last_big_pause_time_left").as_uint();
			_lastMiniPauseTimeLeft = node.attribute(L"last_mini_pause_time_left").as_uint();
		}
		else if (wcscmp(name, L"big_pause") == 0)
		{
			bool enabled = node.attribute(L"enabled").as_bool();
			_enableBigPause = enabled;
			
			int interval = node.attribute(L"interval").as_int();
			_bigPauseInterval = interval;
			
			int duration = node.attribute(L"duration").as_int();
			_bigPauseDuration = duration;
		}
		else if (wcscmp(name, L"mini_pause") == 0)
		{
			bool enabled = node.attribute(L"enabled").as_bool();
			_enableMiniPause = enabled;
			
			int interval = node.attribute(L"interval").as_int();
			if (interval < 5) // correction for old version
				interval = 5;
			_miniPauseInterval = interval;

			int duration = node.attribute(L"duration").as_int();
			if (duration == 0)
				duration = 8;
			_miniPauseDuration = duration;
		}
		else if (wcscmp(name, L"warning") == 0)
		{
			bool enabled = node.attribute(L"enabled").as_bool();
			_enableWarning = enabled;

			float interval = node.attribute(L"interval").as_float();
			_warningInterval = interval;
		}
		else if (wcscmp(name, L"sounds") == 0)
		{
			bool enabled = node.attribute(L"enabled").as_bool();
			_enableSounds = enabled;
		}
		else if (wcscmp(name, L"strict_mode") == 0)
		{
			bool enabled = node.attribute(L"enabled").as_bool();
			_enableStrictMode = enabled;
		}
		else if (wcscmp(name, L"window_nearby") == 0)
		{
			bool enabled = node.attribute(L"enabled").as_bool();
			_settingWindowNearby = enabled;
		}
		else if (wcscmp(name, L"can_close_notifications") == 0)
		{
			bool enabled = node.attribute(L"enabled").as_bool();
			_settingCanCloseNotifications = enabled;
		}
	}
	return true;
}

void EyeApp::CheckSettings()
{
	if (_bigPauseInterval > 120)
	{
		LogMsg("CheckSettings: bigPauseInterval corrected");
		assert(false);

		_bigPauseInterval = 120; // in minutes
	}
	
	if (_bigPauseDuration > 30)
	{
		LogMsg("CheckSettings: bigPauseDuration corrected");
		assert(false);

		_bigPauseDuration = 30;
	}
	
	if (_warningInterval > 3.0f)
	{
		LogMsg("CheckSettings: warningInterval corrected");
		assert(false);

		_warningInterval = 3.0f;
	}
	
	if (_miniPauseInterval > 30)
	{
		LogMsg("CheckSettings: miniPauseInterval corrected");
		assert(false);

		_miniPauseInterval = 30;
	}

	if (_miniPauseDuration > 20)
	{
		LogMsg("CheckSettings: miniPauseDuration corrected");
		assert(false);

		_miniPauseDuration = 20;
	}
	
	if (_timeLeftToBigPause > 1000 * 60 * _bigPauseInterval)
	{
		LogMsg("CheckSettings: _timeLeftToBigPause corrected");
		assert(false);

		_timeLeftToBigPause = 1000 * 60 * _bigPauseInterval;
	}

	if (_timeLeftToMiniPause > 1000 * 60 * _miniPauseInterval)
	{
		LogMsg("CheckSettings: _timeLeftToMiniPause corrected");
		assert(false);

		_timeLeftToMiniPause = 1000 * 60 * _miniPauseInterval;
	}

	//long _relaxingTimeLeft;
	//long _fullscreenBlockDuration;
	//long _inactivityTime;
}

void EyeApp::SaveSettings()
{
	_lastBigPauseTimeLeft = _timeLeftToBigPause;
	_lastMiniPauseTimeLeft = _timeLeftToMiniPause;

	///
	LogMsg("SaveSettings");
	
	pugi::xml_document doc;
	pugi::xml_node node = doc.append_child(pugi::node_element);
	node.set_name(L"settings");
	
	pugi::xml_node nodeStatistics = node.append_child(pugi::node_element);
	nodeStatistics.set_name(L"statistics");
	nodeStatistics.append_attribute(L"first_launch") = _firstLaunch;
	nodeStatistics.append_attribute(L"seen_settings") = _seenSettingsWindow;
	nodeStatistics.append_attribute(L"long_break_count") = _userLongBreakCount;
	nodeStatistics.append_attribute(L"early_skip_count") = _userEarlySkipCount;
	nodeStatistics.append_attribute(L"late_skip_count") = _userLateSkipCount;
	nodeStatistics.append_attribute(L"refuse_count") = _userRefuseCount;
	nodeStatistics.append_attribute(L"postpone_count") = _userPostponeCount;
	nodeStatistics.append_attribute(L"auto_break_count") = _userAutoBreakCount;
	nodeStatistics.append_attribute(L"short_break_count") = _userShortBreakCount;
	if (_lastShutdown.IsValid())
		nodeStatistics.append_attribute(L"last_shutdown") = _lastShutdown.FormatISOCombined().c_str();

	if (_lastBigPauseTimeLeft > INT_MAX)
		_lastBigPauseTimeLeft = INT_MAX;
	nodeStatistics.append_attribute(L"last_big_pause_time_left") = (int)(_lastBigPauseTimeLeft);

	if (_lastMiniPauseTimeLeft > INT_MAX)
		_lastMiniPauseTimeLeft = INT_MAX;
	nodeStatistics.append_attribute(L"last_mini_pause_time_left") = (int)(_lastMiniPauseTimeLeft);
	
	pugi::xml_node nodeBigPause = node.append_child(pugi::node_element);
	nodeBigPause.set_name(L"big_pause");
	nodeBigPause.append_attribute(L"enabled") = GetBigPauseEnabled();
	nodeBigPause.append_attribute(L"interval") = GetBigPauseInterval();
	nodeBigPause.append_attribute(L"duration") = GetBigPauseDuration();
	
	pugi::xml_node nodeMiniPause = node.append_child(pugi::node_element);
	nodeMiniPause.set_name(L"mini_pause");
	nodeMiniPause.append_attribute(L"enabled") = GetMiniPauseEnabled();
	nodeMiniPause.append_attribute(L"interval") = GetMiniPauseInterval();
	nodeMiniPause.append_attribute(L"duration") = GetMiniPauseDuration();
	
	pugi::xml_node nodeWarning = node.append_child(pugi::node_element);
	nodeWarning.set_name(L"warning");
	nodeWarning.append_attribute(L"enabled") = GetWarningEnabled();
	nodeWarning.append_attribute(L"interval") = GetWarningInterval();
	
	pugi::xml_node nodeSounds = node.append_child(pugi::node_element);
	nodeSounds.set_name(L"sounds");
	nodeSounds.append_attribute(L"enabled") = GetSoundsEnabled();
	
	pugi::xml_node nodeStrictMode = node.append_child(pugi::node_element);
	nodeStrictMode.set_name(L"strict_mode");
	nodeStrictMode.append_attribute(L"enabled") = GetStrictModeEnabled();
	
	pugi::xml_node nodeWindowNearby = node.append_child(pugi::node_element);
	nodeWindowNearby.set_name(L"window_nearby");
	nodeWindowNearby.append_attribute(L"enabled") = GetWindowNearbySetting();

	pugi::xml_node nodeCanCloseNotifications = node.append_child(pugi::node_element);
	nodeCanCloseNotifications.set_name(L"can_close_notifications");
	nodeCanCloseNotifications.append_attribute(L"enabled") = GetCanCloseNotificationsSetting();
	
	doc.save_file(GetSavePath() + L"settings.xml", L"\t");
}

void EyeApp::ResetSettings()
{
	_enableBigPause = true;
	_bigPauseInterval = 50;
	_bigPauseDuration = 5;
	_enableMiniPause = true;
	_miniPauseInterval = 10;
	_miniPauseDuration = 8;
	_enableWarning = true;
	_warningInterval = 0.5f;
	_enableSounds = true;
	_enableStrictMode = false;
	_settingWindowNearby = true;
	_settingCanCloseNotifications = false;
	_firstLaunch = true;
	_seenSettingsWindow = false;
}

void EyeApp::ApplySettings()
{
	LogMsg("ApplySettings");

	RestartBigPauseInterval();
	RestartMiniPauseInterval();
	UpdateTaskbarText();
}

void EyeApp::OnSettingsClosed()
{
	LogMsg("OnSettingsClosed");
	
	_settingsWnd = 0;
	
	if (_enableBigPause)
	{
		int newInterval = _bigPauseInterval * 1000 * 60;
		if (newInterval < _timeLeftToBigPause)
			_timeLeftToBigPause = newInterval;
		
		if (_timeLeftToBigPause == 0)
			RestartBigPauseInterval();
	}
	else
	{
		if (_timeLeftToBigPause > 0)
		{
			_timeLeftToBigPause = 0;
			UpdateTaskbarText();
		}
	}

	if (_enableMiniPause)
	{
		int newInterval = _miniPauseInterval * 1000 * 60;
		if (newInterval < _timeLeftToMiniPause)
			_timeLeftToMiniPause = newInterval;

		if (_timeLeftToMiniPause == 0)
		{
			RestartMiniPauseInterval();
			if (GetNextState() == STATE_SUSPENDED)
				ChangeState(STATE_IDLE, 1000);
		}
	}
	else
	{
		if (_timeLeftToMiniPause > 0)
			_timeLeftToMiniPause = 0;
	}

	LogMsg(wxString::Format("    _timeLeftToMiniPause = %d, _timeLeftToBigPause = %d", _timeLeftToMiniPause, _timeLeftToBigPause));
}

void EyeApp::TogglePausedMode(int minutes)
{
	if (GetNextState() == STATE_IDLE)
	{
		// pause for an hour
		_inactivityTime = minutes * 1000 * 60; // in ms
		ChangeState(STATE_SUSPENDED, 500);
		UpdateTaskbarText();

		if (_notificationWnd)
			_notificationWnd->Hide();
	}
	else if (GetNextState() == STATE_SUSPENDED)
	{
		RestartBigPauseInterval();
		RestartMiniPauseInterval();
		UpdateTaskbarText();

		SaveSettings();
	}
}

bool EyeApp::isPausedMode() const
{
	return GetNextState() == STATE_SUSPENDED;
}

void EyeApp::TakeLongBreakNow()
{
	int state = GetNextState();
	
	if (state == STATE_IDLE ||
		state == STATE_AUTO_RELAX ||
		state == STATE_RELAXING)
	{
		StartBigPause();
	}

	if (_notificationWnd)
	{
		_notificationWnd->Hide();
		_notificationWnd = nullptr;
	}

	CloseWaitingWnd();
}

void EyeApp::Exit()
{
	if (!_miniPauseWnds.empty())
	{
		for (std::vector<MiniPauseWindow *>::iterator it = _miniPauseWnds.begin(); it != _miniPauseWnds.end(); ++it)
		{
			MiniPauseWindow * wnd = (*it);
			wnd->Destroy();
		}
		_miniPauseWnds.resize(0);

		//signal to call Exit afterwards
		ChangeState(STATE_DESTROY, 50); // still unstable
	}
	else if (!_bigPauseWnds.empty())
	{
		for (std::vector<BigPauseWindow *>::iterator it = _bigPauseWnds.begin(); it != _bigPauseWnds.end(); ++it)
		{
			BigPauseWindow * wnd = (*it);
			wnd->Destroy();
		}
		_bigPauseWnds.resize(0);
	}
	else
	{
		wxApp::Exit();
	}
}

int EyeApp::OnExit()
{
	LogMsg("OnExit");
	
	static int destroyCount = 0;
	if (destroyCount)
		return 0;
	destroyCount++;
	
	_lastShutdown = wxDateTime::Now();
	SaveSettings();

	_finished = true;

	DeletePendingEvents();
	
	UninstallActivityMonitor();
	_taskBarIcon->RemoveIcon();
	
	Stop();
	
	int res = wxApp::OnExit();
	LogMsg("done OnExit");
	return res;
}

void EyeApp::OnQueryEndSession(wxCloseEvent &evt)
{
	LogMsg("OnQueryEndSession");
	
	DeletePendingEvents();
	getApp()->Exit();
	
	wxApp::OnQueryEndSession(evt);
	
	LogMsg("done OnQueryEndSession");
}

void EyeApp::OnEndSession(wxCloseEvent &evt)
{
	LogMsg("OnEndSession");
	
	UninstallActivityMonitor();
	_taskBarIcon->RemoveIcon();
	
	Stop();
	
	wxApp::OnEndSession(evt);
	
	LogMsg("done OnEndSession");
}


///////////////////////////////////////////////////////////////////////////////////////

EyeTaskBarIcon::EyeTaskBarIcon() : 
	wxTaskBarIcon(),
	_menu(0)
{
	_icon = new wxIcon(L"icon.ico", wxBITMAP_TYPE_ICO, 16, 16);
	if (!_icon->IsOk()) // case for larger fonts
	{
		delete _icon;
		_icon = new wxIcon(L"icon.ico", wxBITMAP_TYPE_ICO);
	}

	_iconGray = new wxIcon(L"icongray.ico", wxBITMAP_TYPE_ICO, 16, 16);
	if (!_iconGray->IsOk()) // case for larger fonts
	{
		delete _iconGray;
		_iconGray = new wxIcon(L"icongray.ico", wxBITMAP_TYPE_ICO);
	}

	_iconSettings = new wxIcon(L"settings.ico", wxBITMAP_TYPE_ICO, 16, 16);
	_iconPause = new wxIcon(L"pause.ico", wxBITMAP_TYPE_ICO, 16, 16);
	_iconResume = new wxIcon(L"resume.ico", wxBITMAP_TYPE_ICO, 16, 16);

	if (!SetIcon(*_icon, langPack->get("tb_popup_default")))
		wxMessageBox(wxT("Could not set icon."));
	
	Connect(wxEVT_TASKBAR_LEFT_DOWN,
		wxMouseEventHandler(EyeTaskBarIcon::OnLeftButtonDown));
	Connect(ID_TASKBAR_MENU_QUIT, wxEVT_COMMAND_MENU_SELECTED,
		wxCommandEventHandler(EyeTaskBarIcon::OnQuit));
	Connect(ID_TASKBAR_MENU_SETTINGS, wxEVT_COMMAND_MENU_SELECTED,
		wxCommandEventHandler(EyeTaskBarIcon::OnSettings));
	Connect(ID_TASKBAR_MENU_PAUSE_RESUME_MONITORING, wxEVT_COMMAND_MENU_SELECTED,
		wxCommandEventHandler(EyeTaskBarIcon::OnPauseResumeMonitoring));
	Connect(ID_TASKBAR_MENU_PAUSE_RESUME_MONITORING_2, wxEVT_COMMAND_MENU_SELECTED,
		wxCommandEventHandler(EyeTaskBarIcon::OnPauseResumeMonitoring2));
	Connect(ID_TASKBAR_MENU_TAKE_LONG_BREAK_NOW, wxEVT_COMMAND_MENU_SELECTED,
		wxCommandEventHandler(EyeTaskBarIcon::OnTakeLongBreakNow));
	
}

void EyeTaskBarIcon::UpdateTooltip(wxString const &text)
{
	if (!getApp()->isPausedMode())
		SetIcon(*_icon, text);
	else
		SetIcon(*_iconGray, text);
}

void EyeTaskBarIcon::OnLeftButtonDown(wxMouseEvent /*wxTaskBarIconEvent*/& WXUNUSED(event))
{
    wxMenu *menu = CreatePopupMenu();
    if (menu)
    {
        PopupMenu(menu);
        delete menu;
    }
}

wxMenu * EyeTaskBarIcon::CreatePopupMenu()
{
	_menu = new wxMenu();
	
	wxMenuItem *item = new wxMenuItem(_menu, ID_TASKBAR_MENU_SETTINGS, langPack->get("tb_menu_settings"), langPack->get("tb_menu_settings_tip"));
	item->SetBitmap(*_iconSettings);
	_menu->Append(item);
	
	if (getApp()->GetNextState() == STATE_SUSPENDED)
	{
		item = new wxMenuItem(_menu, ID_TASKBAR_MENU_PAUSE_RESUME_MONITORING, langPack->get("tb_menu_resume_monitoring"), langPack->get("tb_menu_resume_monitoring_tip"));
		item->SetBitmap(*_iconResume);
		_menu->Append(item);
	}
	else
	{
		wxMenu* subMenu = new wxMenu();

		item = _menu->AppendSubMenu(subMenu, langPack->get("tb_menu_pause_monitoring"));
		//item->SetBitmap(*_iconPause);

		wxMenuItem *subitem = new wxMenuItem(_menu, ID_TASKBAR_MENU_PAUSE_RESUME_MONITORING, langPack->get("tb_menu_pause_monitoring_1"), langPack->get("tb_menu_pause_monitoring_tip"));
		subitem->SetBitmap(*_iconPause);
		subMenu->Append(subitem);

		subitem = new wxMenuItem(_menu, ID_TASKBAR_MENU_PAUSE_RESUME_MONITORING_2, langPack->get("tb_menu_pause_monitoring_2"), langPack->get("tb_menu_pause_monitoring_tip"));
		subitem->SetBitmap(*_iconPause);
		subMenu->Append(subitem);

		item = new wxMenuItem(_menu, ID_TASKBAR_MENU_TAKE_LONG_BREAK_NOW, langPack->get("tb_menu_take_break_now"));
		//item->SetBitmap(*_iconSettings);
		_menu->Append(item);
	}

	_menu->AppendSeparator();
	
	item = new wxMenuItem(_menu, ID_TASKBAR_MENU_QUIT, langPack->get("tb_menu_quit"), langPack->get("tb_menu_quit_tip"));
	_menu->Append(item);

	return _menu;
}

void EyeTaskBarIcon::OnPauseResumeMonitoring(wxCommandEvent &)
{
	getApp()->TogglePausedMode(60);
}

void EyeTaskBarIcon::OnPauseResumeMonitoring2(wxCommandEvent &)
{
	getApp()->TogglePausedMode(180);
}

void EyeTaskBarIcon::OnTakeLongBreakNow(wxCommandEvent &)
{
	getApp()->TakeLongBreakNow();
}

void EyeTaskBarIcon::OnQuit(wxCommandEvent &)
{
	getApp()->Exit();
}

void EyeTaskBarIcon::OnSettings(wxCommandEvent &)
{
	getApp()->OpenSettings();
}
