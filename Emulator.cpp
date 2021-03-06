//#include "App.xaml.h"
#include "Emulator.h"
#include "EmulatorFileHandler.h"
#include "EmulatorSettings.h"
#include <d3d11_1.h>
#include <GBA.h>
#include <Util.h>
#include <SoundDriver.h>
#include "App.xaml.h"


using namespace Platform;
using namespace concurrency;

using namespace Platform;
using namespace Windows::Foundation;

extern int emulating;
extern void ContinueEmulation(void);

extern SoundDriver *soundDriver;
extern long  soundSampleRate;

extern void soundSetVolume(float);

CRITICAL_SECTION pauseSync;


namespace VBA10
{
	extern bool timeMeasured;
	bool autosaving;
	
	EmulatedSystem EmulatorGame::emulator = GBASystem;

	EmulatorGame *EmulatorGame::instance = nullptr;

	EmulatorGame *EmulatorGame::GetInstance(void)
	{
		return EmulatorGame::instance;
	}

	EmulatorGame::EmulatorGame(bool restore) 
		: restoreState(restore),
		graphicsResourcesReleased(false), stopThread(false),
		updateCount(0), frameSkipped(0), gfxbuffer(nullptr), threadAction(nullptr), xboxElapsed(3601.0f)
	{
		EmulatorGame::instance = this;
	}

	EmulatorGame::~EmulatorGame(void)
	{
		//this->threadAction->Cancel();
		this->stopThread = true;
		ContinueEmulation();
		WaitForSingleObjectEx(this->flipBufferEvent, INFINITE, false);
		this->threadAction = nullptr;

		CloseHandle(this->updateEvent);
		CloseHandle(this->flipBufferEvent);
		CloseHandle(this->sleepEvent);
		DeleteCriticalSection(&pauseSync);


		if(this->gfxbuffer)
		{
			delete [] this->gfxbuffer;
			this->gfxbuffer = nullptr;
		}

		this->ReleaseAllResources();

		delete [] this->gfxbuffer;

	}

	void EmulatorGame::ReleaseAllResources(void)
	{
#ifndef NO_XBOX
		delete this->p1Controller;
#endif
		delete this->keyboard;
		delete this->virtualInput;

		this->DeInitSound();
		
		this->graphicsResourcesReleased = true;
	}
	
	void EmulatorGame::Start(void)
	{
		SetEvent(this->updateEvent);
	}


	void EmulatorGame::Initialize()
	{
#ifndef NO_XBOX
		this->p1Controller = new ControllerInput(1);
#endif
		this->keyboard = new KeyboardInput();
		this->virtualInput = new VirtualControllerInput();
		this->HidInput = ref new HIDControllerInput();


		this->updateCount = 0;
		this->frameSkipped = false;


		if(!this->graphicsResourcesReleased)
		{
			this->InitSound();
		
			systemColorDepth = 32;
			systemRedShift = 19;
			systemBlueShift = 3;
			systemGreenShift = 11;

			utilUpdateSystemColorMaps();

			if(this->restoreState)
			{
				this->restoreState = false;
				RestoreFromApplicationDataAsync();
			}

			RestoreSettings();

			// Start Snes9x Thread
			InitializeCriticalSectionEx(&pauseSync, NULL, NULL);
			this->flipBufferEvent = CreateEventEx(NULL, NULL, CREATE_EVENT_INITIAL_SET, EVENT_ALL_ACCESS);
			this->updateEvent = CreateEventEx(NULL, NULL, NULL, EVENT_ALL_ACCESS);
			this->sleepEvent = CreateEventEx(NULL, NULL, NULL, EVENT_ALL_ACCESS);
			this->threadAction = ThreadPool::RunAsync(ref new WorkItemHandler([this](IAsyncAction ^action)
			{
				this->UpdateAsync();	
			}), WorkItemPriority::High, WorkItemOptions::None);
		}else
		{
			/*ResetEvent(this->updateEvent);
			SetEvent(this->flipBufferEvent);*/
			ResetEvent(this->flipBufferEvent);
			ResetEvent(this->updateEvent);
		}

		this->graphicsResourcesReleased = false;
	}

	void EmulatorGame::StartEmulatorThread()
	{
		this->threadAction = ThreadPool::RunAsync(ref new WorkItemHandler([this](IAsyncAction ^action)
		{
			this->UpdateAsync();
		}), WorkItemPriority::High, WorkItemOptions::None);
	}

	void EmulatorGame::StopEmulatorThread()
	{
		if (this->threadAction)
		{
			this->threadAction->Cancel();
			this->threadAction->Close();
		}
	}

	void EmulatorGame::InitSound(void)
	{
		if(soundDriver)
		{
			soundDriver->init(soundSampleRate);
			if(true)//EmulatorSettings::Current->SoundEnabled)
			{
				soundSetVolume(1.0f);
			}else
			{
				soundSetVolume(0.0f);
			}
		}
	}
		
	void EmulatorGame::DeInitSound(void)
	{
		if(soundDriver)
		{
			soundDriver->close();
		}
	}

	VirtualControllerInput *EmulatorGame::GetVirtualController(void) const
	{
		return this->virtualInput;
	}
	
	KeyboardInput *EmulatorGame::GetKeyboardInput(void) const
	{
		return this->keyboard;
	}
#ifndef NO_XBOX
	ControllerInput *EmulatorGame::GetControllerInput(void) const
	{
		return this->p1Controller;
	}
#endif

	HIDControllerInput ^EmulatorGame::GetHidControllerInput(void) const
	{
		return this->HidInput;
	}

	void EmulatorGame::FocusChanged(bool focus)
	{
		this->focus = focus;
	}

	void uSleep(int waitTime) 
	{
		__int64 time1 = 0, time2 = 0, freq = 0;

		QueryPerformanceCounter((LARGE_INTEGER *) &time1);
		QueryPerformanceFrequency((LARGE_INTEGER *)&freq);

		do {
			QueryPerformanceCounter((LARGE_INTEGER *) &time2);
		} while((time2-time1) < waitTime);
	}

	void EmulatorGame::ResizeBuffer(float width, float height)
	{
		this->width = width;
		this->height = height;
		this->virtualInput->UpdateVirtualControllerRectangles();
	}

	task<void> EmulatorGame::StopROMAsync(void)
	{
		return create_task([this]()
		{
			if(IsROMLoaded())
			{

				this->Pause();
				//this->InitSound();
				ROMFile = nullptr;
				ROMFolder = nullptr;
				ROMLoaded = false;
				this->frameSkipped = false;
				this->updateCount = 0;
			}
		});
	}

	void EmulatorGame::Pause(void)
	{

		if(emulating)
		{
			EnterCriticalSection(&pauseSync);
			emulating = false;
		
		}

	}

	void EmulatorGame::Unpause(void)
	{
		if(IsROMLoaded() && !emulating)
		{

			emulating = true;

			LeaveCriticalSection(&pauseSync);
		}
	}

	bool EmulatorGame::IsPaused(void)
	{
		//return Settings.StopEmulation;
		return !emulating;
	}
	
	int EmulatorGame::GetWidth(void)
	{
		return this->width;
	}

	int EmulatorGame::GetHeight(void)
	{
		return this->height;
	}

	bool EmulatorGame::LastFrameSkipped(void)
	{
		return this->frameSkipped;
	}

	void EmulatorGame::ResetXboxTimer()
	{
		this->xboxElapsed = 0.0f;
	}

	float EmulatorGame::GetXboxTimer()
	{
		return this->xboxElapsed;
	}

	void EmulatorGame::UpdateAsync(void)
	{
		WaitForSingleObjectEx(this->updateEvent, INFINITE, false);
		
		while(!this->stopThread)
		{			
			EnterCriticalSection(&pauseSync);
			emulator.emuMain(emulator.emuCount);
			LeaveCriticalSection(&pauseSync);

			if (this->stopThread)
				bool test = true;
		}
		SetEvent(this->flipBufferEvent);

#if _DEBUG
		OutputDebugStringW(L"Thread ended.\n");
#endif
	}

	void EmulatorGame::Update(float timeDelta)
	{		
		this->keyboard->Update();
		this->xboxElapsed += timeDelta;

#ifndef NO_XBOX
		if (xboxElapsed < 3600.0f || App::IsPremium)
		{
			this->p1Controller->Update();
			this->HidInput->Update(true);
		}
		else
			this->HidInput->Update(false);
#endif
		this->virtualInput->Update();

		

		//there is no need to update hidcontroller because we use event to update

		/*if(timeMeasured && IsROMLoaded() && (!Settings.StopEmulation || autosaving))
		{		
			Settings.Mute = !SoundEnabled();
			
			if(this->focus)
			{
				this->HandleInput();
			}

			this->FlipBuffers(buffer, rowPitch);
			
		}*/
	}

	void EmulatorGame::FlipBuffers(void *buffer, size_t rowPitch)
	{
		WaitForSingleObjectEx(this->flipBufferEvent, INFINITE, false);

		/*GFX.Screen = (uint16*) (buffer);
		GFX.Pitch = rowPitch;*/

		SetEvent(this->updateEvent);
	}

	bool EmulatorGame::RestoreHidConfig() 
	{

		//return create_task(LoadHidConfig())
		//	.then([this] 
		//{
			//initialize boolean button map
			this->HidInput->booleanControlMapping = ref new Map <int, Platform::String^ >();

			//create list of numeric controls
			this->HidInput->allNumericControls = ref new Vector < HidNumericControlExt^>();

			if (hidConfigs->HasKey(EventHandlerForDevice::Current->DeviceInformation->Id))
			{
				auto config = hidConfigs->Lookup(EventHandlerForDevice::Current->DeviceInformation->Id);

				//transfer boolean control
				for (auto bpair : config->booleanControlMapping)
				{
					int bid = bpair->Key;
					String^ function = bpair->Value;

					this->HidInput->booleanControlMapping->Insert(bid, function);
				}

				//transfer numeric control
				for (auto ncontrol2 : config->allNumericControls)
				{
					HidNumericControlExt^ ncontrol = ref new HidNumericControlExt(ncontrol2->UsagePage, ncontrol2->UsageId);

					//transfer the value
					ncontrol->Type = ncontrol2->Type;
					ncontrol->DefaultValue = ncontrol2->DefaultValue;
					ncontrol->MaximumValue = ncontrol2->MaximumValue;
					for (auto npair : ncontrol2->Mapping)
					{
						int nid = npair->Key;
						String^ function = npair->Value;
						ncontrol->Mapping->Insert(nid, function);
					}

					this->HidInput->allNumericControls->Append(ncontrol);
				}




				return true;
			}
			else
			{
				return false;
			}
		//}, task_continuation_context::use_current());
	}


	void EmulatorGame::EnterButtonEditMode()
	{
		this->virtualInput->EnterEditMode();
	}

	bool EmulatorGame::IsButtonEditMode()
	{
		return this->virtualInput->IsEditMode();
	}

	void EmulatorGame::LeaveButtonEditMode(bool accept)
	{
		this->virtualInput->LeaveEditMode(accept);
	}
}
