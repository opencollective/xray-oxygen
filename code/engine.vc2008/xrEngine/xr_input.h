﻿#pragma once
#include "../FrayBuildConfig.hpp"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

class	ENGINE_API				IInputReceiver;

//\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\\
//описание класса
constexpr int mouse_device_key		= 1;
constexpr int keyboard_device_key	= 2;
constexpr int all_device_key		= mouse_device_key | keyboard_device_key;
constexpr int default_key			= mouse_device_key | keyboard_device_key ;

class ENGINE_API CInput : public pureFrame, public pureAppActivate, public pureAppDeactivate
{
public:
	enum {
		COUNT_MOUSE_BUTTONS			= 8,
		COUNT_MOUSE_AXIS			= 3,
		COUNT_KB_BUTTONS			= 256
	};
#ifndef RAW_INPUT_USE
	struct sxr_mouse
	{
		DIDEVCAPS					capabilities;
		DIDEVICEINSTANCE			deviceInfo;
		DIDEVICEOBJECTINSTANCE		objectInfo;
		u32							mouse_dt;
	};
	struct sxr_key
	{
		DIDEVCAPS					capabilities;
		DIDEVICEINSTANCE			deviceInfo;
		DIDEVICEOBJECTINSTANCE		objectInfo;
	};
#endif
#ifdef RAW_INPUT_USE
	static HRAWINPUT	DataInput;
private:
	LPRAWINPUT			pDI;			// The Input object
	RAWINPUTDEVICE		Mouse;
	RAWINPUTDEVICE		KeyBoard;
#else
private:
	LPDIRECTINPUT8				pDI;			// The DInput object
	LPDIRECTINPUTDEVICE8		pMouse;			// The DIDevice7 interface
	LPDIRECTINPUTDEVICE8		pKeyboard;		// The DIDevice7 interface
#endif
	//----------------------
	u32							timeStamp	[COUNT_MOUSE_AXIS];
	u32							timeSave	[COUNT_MOUSE_AXIS];
	int 						offs		[COUNT_MOUSE_AXIS];
	BOOL						mouseState	[COUNT_MOUSE_BUTTONS];

	//----------------------
	BOOL						KBState		[COUNT_KB_BUTTONS];
#ifdef RAW_INPUT_USE
	HRESULT						CreateInputDevice(RAWINPUTDEVICE* pDev, GUID gDevice, RID_DEVICE_INFO* pDataFormat, u32 dwFlags, u32 bSize);

#else
	HRESULT						CreateInputDevice(LPDIRECTINPUTDEVICE8* device, GUID guidDevice,
													const DIDATAFORMAT* pdidDataFormat, u32 dwFlags, u32 buf_size );
#endif

	xr_vector<IInputReceiver*>	cbStack;

	void						MouseUpdate					( );
	void						KeyUpdate					( );

public:
	u32							dwCurTime;
#ifndef RAW_INPUT_USE
	sxr_mouse					mouse_property;
	sxr_key						key_property;
	
	void						SetAllAcquire				( BOOL bAcquire = TRUE );
	void						SetMouseAcquire				( BOOL bAcquire );
	void						SetKBDAcquire				( BOOL bAcquire );
#endif

	void						iCapture					( IInputReceiver *pc );
	void						iRelease					( IInputReceiver *pc );
	BOOL						iGetAsyncKeyState			( int dik );
	BOOL						iGetAsyncBtnState			( int btn );
	void						iGetLastMouseDelta			( Ivector2& p )	{ p.set(offs[0],offs[1]); }

	CInput						( BOOL bExclusive = true, int deviceForInit = default_key);
	~CInput						( );

	virtual void	_BCL		OnFrame						(void);
	virtual void				OnAppActivate				(void);
	virtual void				OnAppDeactivate				(void);

	IInputReceiver*				CurrentIR					();

public:
			void				exclusive_mode				(const bool &exclusive);
	IC		bool				get_exclusive_mode			();
			void				unacquire					();
			void				acquire						(const bool &exclusive);
			bool				get_dik_name				(int dik, LPSTR dest, int dest_sz);

			void				feedback					(u16 s1, u16 s2, float time);
};

extern ENGINE_API CInput *		pInput;