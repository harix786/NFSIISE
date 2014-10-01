#include "DInput.h"

#include <SDL2/SDL_mouse.h>
#include <SDL2/SDL_timer.h>

#define MOUSE        0x6F1D2B60
#define JOYSTICK     0x6F1D2B70
#define FORCE_CONST  0x13541C20
#define FORCE_SQUARE 0x13541C22
#define FORCE_SPRING 0x13541C27

static BOOL lastMouseButton;
extern BOOL mouseAsJoystick;
extern int32_t joystickAxisValueShift[ 2 ], mouseJoySensitivity;
extern uint32_t joystickAxes[ 2 ][ 8 ], joystickButtons[ 2 ][ 15 ];

#define CONVERT(x)    (((x)*0x7FFF)/10000)

static inline void setEnvelope( uint16_t *attack_length, uint16_t *attack_level, uint16_t *fade_length, uint16_t *fade_level, DIENVELOPE *envelope )
{
	*attack_length = envelope->attackTime / 1000;
	*attack_level = CONVERT( envelope->attackLevel );
	*fade_length = envelope->fadeTime / 1000;
	*fade_level = CONVERT( envelope->fadeLevel );
}
static void setEffect( uint16_t real_type, SDL_HapticEffect *effect, const DIEFFECT *di_eff )
{
	switch ( real_type )
	{
		case SDL_HAPTIC_CONSTANT:
		{
			DICONSTANTFORCE *di_constant = ( DICONSTANTFORCE * )di_eff->typeSpecificParams;
			if ( effect->type == SDL_HAPTIC_SINE )
			{
				SDL_HapticPeriodic *periodic = &effect->periodic;
				if ( di_constant )
					periodic->magnitude = CONVERT( di_constant->magnitude );
				periodic->length = di_eff->duration == SDL_HAPTIC_INFINITY ? SDL_HAPTIC_INFINITY : di_eff->duration / 1000;
// 				printf( "Constant: %d\n", periodic->length );
			}
			else
			{
				SDL_HapticConstant *constant = &effect->constant;
				if ( di_constant )
					constant->level = CONVERT( di_constant->magnitude );
				constant->length = di_eff->duration == SDL_HAPTIC_INFINITY ? SDL_HAPTIC_INFINITY : di_eff->duration / 1000;
// 				printf( "Constant: %d\n", constant->length );
			}
		} break;
		case SDL_HAPTIC_SINE:
		{
			DIPERIODIC *di_periodic = ( DIPERIODIC * )di_eff->typeSpecificParams;

			SDL_HapticPeriodic *periodic = &effect->periodic;

			if ( di_periodic )
			{
				periodic->magnitude = CONVERT( di_periodic->magnitude );
				periodic->offset = CONVERT( di_periodic->offset );
				periodic->period = di_periodic->period / 1000;
				periodic->phase = di_periodic->phase;
			}

			periodic->length = di_eff->duration == SDL_HAPTIC_INFINITY ? SDL_HAPTIC_INFINITY : di_eff->duration / 1000;

			if ( di_eff->envelope )
				setEnvelope( &periodic->attack_length, &periodic->attack_level, &periodic->fade_length, &periodic->fade_level, di_eff->envelope );

// 			printf( "Square: %d\n", periodic->length );
		} break;
		case SDL_HAPTIC_SPRING:
		{
			/* NOT TESTED!!! */
			SDL_HapticCondition *condition = &effect->condition;
			uint32_t i;
			for ( i = 0 ; i < di_eff->cAxes ; ++i )
			{
				DICONDITION *di_condition = ( DICONDITION * )di_eff->typeSpecificParams + i;
				condition->center[ i ] = CONVERT( di_condition->lOffset );
				condition->right_coeff[ i ] = CONVERT( di_condition->lPositiveCoefficient );
				condition->left_coeff[ i ] = CONVERT( di_condition->lNegativeCoefficient );
				condition->right_sat[ i ] = CONVERT( di_condition->dwPositiveSaturation );
				condition->left_sat[ i ] = CONVERT( di_condition->dwNegativeSaturation );
				condition->deadband[ i ] = CONVERT( di_condition->lDeadBand );
// 				printf( "Spring[ %d ]: %d %d %d %d %d %d\n", i, condition->center[ i ], condition->right_coeff[ i ], condition->left_coeff[ i ], condition->right_sat[ i ], condition->left_sat[ i ], condition->deadband[ i ] );
			}

			condition->length = di_eff->duration == SDL_HAPTIC_INFINITY ? SDL_HAPTIC_INFINITY : di_eff->duration / 1000;

// 			printf( "Spring: %X\n", condition->length );
		} break;
	}
}

static STDCALL uint32_t QueryInterface( void **this, const IID *const riid, void **object )
{
	/* Only joystick */
	++( ( DirectInputObject * )( *this - sizeof( DirectInputObject ) ) )->ref;
	*object = this;
// 	printf( "QueryInterface: 0x%X %p\n", (*riid)[0], *this );
	return 0;
}
static STDCALL uint32_t Release( void **this )
{
	DirectInputObject *dinputObj = ( DirectInputObject * )( *this - sizeof( DirectInputObject ) );
	if ( !--dinputObj->ref )
	{
		if ( dinputObj->is_device )
		{
			DirectInputDevice *dinputDev = ( *( DirectInputDevice ** )this );
			uint32_t i;
// 			printf( "Release: close device %p\n", dinputDev->joy );

			for ( i = 0 ; i != dinputDev->num_effects ; ++i )
				free( ( void * )dinputDev->effects[ i ] - sizeof( DirectInputObject ) );
			free( dinputDev->effects );

			SDL_HapticClose( dinputDev->haptic );
#ifdef WIN32
			SDL_JoystickClose( dinputDev->joy ); //SDL2 crashes on closing joystick on Linux... :D
#endif
		}
// 		printf( "Release: free 0x%p\n", *this );
		free( dinputObj );
		free( this );
	}
// 	printf( "Release: 0x%p\n", *this );
	return 0;
}

static STDCALL uint32_t SetParameters( DirectInputEffect **this, const DIEFFECT *eff, uint32_t flags )
{
// 	if ( (*this)->real_type != SDL_HAPTIC_SPRING )
// 		printf( "SetParameters: %X %X\n", flags, (*this)->real_type );
	if ( (*this)->haptic )
	{
		setEffect( (*this)->real_type, &(*this)->effect, eff );
		if ( !SDL_HapticUpdateEffect( (*this)->haptic, (*this)->effect_idx, &(*this)->effect ) )
		{
			if ( (*this)->effect.type == SDL_HAPTIC_SINE && (*this)->real_type == SDL_HAPTIC_CONSTANT )
				SDL_HapticRunEffect( (*this)->haptic, (*this)->effect_idx, 1 );
		}
	}
	return 0;
}
static STDCALL uint32_t Start( DirectInputEffect **this, uint32_t iterations, uint32_t flags )
{
// 	if ( (*this)->real_type != SDL_HAPTIC_SPRING )
// 		printf( "Start: %X\n", (*this)->real_type );
	SDL_HapticRunEffect( (*this)->haptic, (*this)->effect_idx, iterations );
	return 0;
}
static STDCALL uint32_t Stop( DirectInputEffect **this )
{
// 	if ( (*this)->real_type != SDL_HAPTIC_SPRING )
// 		printf( "Stop: %X\n", (*this)->real_type );
	SDL_HapticStopEffect( (*this)->haptic, (*this)->effect_idx );
	return 0;
}
static STDCALL uint32_t Download( DirectInputEffect **this )
{
// 	printf( "Download: %X\n", (*this)->real_type );
	return 0;
}
static STDCALL uint32_t Unload( DirectInputEffect **this )
{
	SDL_HapticStopEffect( (*this)->haptic, (*this)->effect_idx );
	return 0;
}

static STDCALL uint32_t GetCapabilities( DirectInputDevice **this, DIDEVCAPS *devCaps )
{
	/* Only joystick */
	if ( (*this)->guid.a == JOYSTICK )
	{
	// 	printf( "GetCapabilities %p\n", *this );
		memset( devCaps, 0, sizeof( DIDEVCAPS ) );
		if ( (*this)->joy )
		{
			if ( (*this)->haptic )
				devCaps->flags = 0x100; //DIDC_FORCEFEEDBACK
			devCaps->buttons = SDL_JoystickNumButtons( (*this)->joy );
			devCaps->axes = SDL_JoystickNumAxes( (*this)->joy );
			if ( devCaps->buttons > 15 )
				devCaps->buttons = 15;
			if ( devCaps->axes > 4 )
				devCaps->axes = 4;
		}
		else //Mouse as joystick
		{
			devCaps->buttons = 5;
			devCaps->axes = 2;
		}
	}
	return 0;
}
static STDCALL uint32_t SetProperty( DirectInputDevice **this, const GUID *const rguidProp, const DIPROPHEADER *pdiph )
{
	if ( ( uint32_t )rguidProp == 7 /*DIPROP_FFGAIN*/ )
		SDL_HapticSetGain( (*this)->haptic, ( ( const DIPROPDWORD * )pdiph )->dwData / 100 );
	return 0;
}
static STDCALL uint32_t Acquire( DirectInputDevice **this )
{
// 	printf( "Acquire: %p %X\n", *this );
	return 0;
}
static STDCALL uint32_t Unacquire( DirectInputDevice **this )
{
// 	printf( "Unacquire: %u\n", (*this)->ref );
	return 0;
}
static STDCALL uint32_t GetDeviceState( DirectInputDevice **this, uint32_t cbData, void *data )
{
	/* Only joystick */
	if ( data && cbData == sizeof( DIJOYSTATE ) && (*this)->guid.a == JOYSTICK )
	{
		uint32_t i;

		DIJOYSTATE *joyState = ( DIJOYSTATE * )data;
		SDL_memset4( joyState->axes, 0x7FFF, 6 );
		memset( joyState->buttons, 0, sizeof joyState->buttons );

		SDL_Joystick *joy = (*this)->joy;
		if ( joy )
		{
			uint32_t numButtons = SDL_JoystickNumButtons( joy );
			uint32_t numAxes = SDL_JoystickNumAxes( joy );
			uint32_t joyIdx = (*this)->guid.b - 1;
			uint32_t j;

			if ( numButtons > 15 )
				numButtons = 15;
			if ( numAxes > 4 )
				numAxes = 4;

// 			for ( ;; )
// 			{
// 				printf( "%d\n", SDL_JoystickGetAxis( joy, 0 ) );
// 				SDL_Delay( 10 );
// 			}

			for ( i = 0 ; i < numButtons ; ++i )
				joyState->buttons[ i ] = SDL_JoystickGetButton( joy, joystickButtons[ joyIdx ][ i ] ) << 7;
			for ( i = 0 ; i < numAxes ; ++i )
			{
				j = i < 3 ? i : 5;
				joyState->axes[ j ] = ( uint16_t )SDL_JoystickGetAxis( joy, joystickAxes[ joyIdx ][ i ] ) ^ 0x8000;
				if ( joystickAxes[ joyIdx ][ i + 4 ] )
					joyState->axes[ j ] = ( joyState->axes[ j ] >> 1 ) + 32768;
			}
			if ( joystickAxisValueShift[ joyIdx ] )
			{
				if ( joyState->axes[ 0 ] < 0x8000 )
					joyState->axes[ 0 ] -= joystickAxisValueShift[ joyIdx ];
				else if ( joyState->axes[ 0 ] > 0x8000 )
					joyState->axes[ 0 ] += joystickAxisValueShift[ joyIdx ];

				if ( joyState->axes[ 0 ] > 0xFFFF )
					joyState->axes[ 0 ] = 0xFFFF;
				else if ( joyState->axes[ 0 ] < 0x0000 )
					joyState->axes[ 0 ] = 0x0000;
			}

// 			static int lastT;
// 			int32_t t = SDL_GetTicks();
// 			if ( t - lastT >= 10 )
// 			{
// 				printf( "%X\n", joyState->axes[ 0 ] );
// 				lastT = t;
// 			}
		}
		else //Mouse as joystick
		{
			static int32_t x, y, lastT, lastTDiff;
			int32_t t = SDL_GetTicks();
			int32_t tDiff = t - lastT;
			BOOL readAxes = tDiff >= 10;
			uint32_t buttons = SDL_GetRelativeMouseState( readAxes ? &x : NULL, readAxes ? &y : NULL );
			joyState->buttons[ 0 ] = ( buttons & SDL_BUTTON_LMASK ) ? 0x80 : 0;
			joyState->buttons[ 1 ] = ( buttons & SDL_BUTTON_RMASK ) ? 0x80 : 0;
			joyState->buttons[ 2 ] = ( buttons & SDL_BUTTON_MMASK ) ? 0x80 : 0;
			joyState->buttons[ 3 ] = ( buttons & 0x80  ) ? 0x80 : 0;
			joyState->buttons[ 4 ] = ( buttons & 0x100 ) ? 0x80 : 0;
			if ( readAxes )
			{
				lastT = t;
				lastTDiff = tDiff;
			}
			joyState->axes[ 0 ] = 0x7FFF + x * lastTDiff * mouseJoySensitivity;
			joyState->axes[ 1 ] = 0x7FFF + y * lastTDiff * mouseJoySensitivity;
			for ( i = 0 ; i < 2 ; ++i )
			{
				if ( joyState->axes[ i ] < 0 )
					joyState->axes[ i ] = 0;
				else if ( joyState->axes[ i ] > 0xFFFF )
					joyState->axes[ i ] = 0xFFFF;
			}
// 			if ( readAxes )
// 				printf( "%X\n", joyState->axes[ 0 ] );
		}
	}
	return 0;
}
static STDCALL uint32_t GetDeviceData( DirectInputDevice **this, uint32_t cbObjectData, DIDEVICEOBJECTDATA *rgdod, uint32_t *pdwInOut, uint32_t dwFlags )
{
	/* Only mouse */
	if ( rgdod && (*this)->guid.a == MOUSE )
	{
		int x, y;
		uint32_t buttons;
		memset( rgdod, 0, *pdwInOut * sizeof( DIDEVICEOBJECTDATA ) );
		if ( !mouseAsJoystick )
		{
			buttons = SDL_GetRelativeMouseState( &x, &y );
			BOOL mouseButton = buttons & SDL_BUTTON_LMASK;

			rgdod[ 0 ].dwData = x;
			rgdod[ 0 ].dwOfs = 0;
			rgdod[ 1 ].dwData = y;
			rgdod[ 1 ].dwOfs = 4;
			if ( !lastMouseButton )
			{
				rgdod[ 2 ].dwData = -mouseButton;
				rgdod[ 2 ].dwOfs = 12;
			}
			lastMouseButton = mouseButton;
		}
	}
// 	putchar( '\n' );
	return 0;
}
static STDCALL uint32_t SetDataFormat( DirectInputDevice **this, const DIDATAFORMAT *df )
{
	/* NFSIISE uses standard data format:
	 * 	Mouse    - c_dfDIMouse
	 * 	Joystick - c_dfDIJoystick
	*/
	return 0;
}
static STDCALL uint32_t SetEventNotification( DirectInputDevice **this, void *hEvent )
{
// 	printf( "SetEventNotification: %p 0x%p\n", *this, hEvent );
	return 0;
}
static STDCALL uint32_t SetCooperativeLevel( DirectInputDevice **this, void *hwnd, uint32_t dwFlags )
{
// 	printf( "SetCooperativeLevel: %p %p 0x%X\n", *this, hwnd, dwFlags );
	return 0;
}
static STDCALL uint32_t CreateEffect( DirectInputDevice **this, const GUID *const rguid, const DIEFFECT *eff, DirectInputEffect ***deff, void *punkOuter )
{
	/* Only joystick */
	DirectInputEffect *dinputEff = ( DirectInputEffect * )calloc( 1, sizeof( DirectInputObject ) + sizeof( DirectInputEffect ) );
	( ( DirectInputObject * )dinputEff )->ref = 1;
	dinputEff = ( void * )dinputEff + sizeof( DirectInputObject );

	dinputEff->SetParameters = SetParameters;
	dinputEff->Start = Start;
	dinputEff->Stop = Stop;
	dinputEff->Download = Download;
	dinputEff->Unload = Unload;

	memcpy( &dinputEff->guid, rguid, sizeof( GUID ) );

	switch ( rguid->a )
	{
		case FORCE_CONST:
			dinputEff->effect.type = ( SDL_HapticQuery( (*this)->haptic ) & SDL_HAPTIC_CONSTANT ) ? SDL_HAPTIC_CONSTANT : SDL_HAPTIC_SINE;
			dinputEff->real_type = SDL_HAPTIC_CONSTANT;
			break;
		case FORCE_SQUARE:
			dinputEff->real_type = dinputEff->effect.type = SDL_HAPTIC_SINE;
			break;
		case FORCE_SPRING:
			dinputEff->real_type = dinputEff->effect.type = SDL_HAPTIC_SPRING;
			break;
	}
	setEffect( dinputEff->real_type, &dinputEff->effect, eff );
	dinputEff->effect_idx = SDL_HapticNewEffect( (*this)->haptic, &dinputEff->effect );

// 	printf( "%X %d %s\n", dinputEff->guid.a, dinputEff->effect_idx, dinputEff->effect_idx == -1 ? SDL_GetError() : "" );

	if ( dinputEff->effect_idx != -1 )
		dinputEff->haptic = (*this)->haptic;

	*deff = malloc( sizeof( void * ) );
	**deff = dinputEff;

	(*this)->effects = ( DirectInputEffect ** )realloc( (*this)->effects, ++(*this)->num_effects * sizeof( DirectInputEffect * ) );
	(*this)->effects[ (*this)->num_effects - 1 ] = dinputEff;

	return 0;
}
static STDCALL uint32_t GetObjectInfo( DirectInputDevice **this, DIDEVICEOBJECTINSTANCEA *pdidoi, uint32_t dwObj, uint32_t dwHow )
{
	/* Only joystick */
	memset( pdidoi, 0, sizeof( DIDEVICEOBJECTINSTANCEA ) );
// 	printf( "GetObjectInfo: %p %d %d\n", *this, dwObj, dwHow );
	return 0;
}
static STDCALL uint32_t SendForceFeedbackCommand( DirectInputDevice **this, uint32_t flags )
{
	/* Only joystick */
// 	printf( "SendForceFeedbackCommand: %X\n", flags );
	switch ( flags )
	{
		case 0x01: //DISFFC_RESET
		case 0x02: //DISFFC_STOPALL
			SDL_HapticStopAll( (*this)->haptic );
			break;
		case 0x10: //DISFFC_SETACTUATORSON
// 			SDL_HapticUnpause( (*this)->haptic );
			break;
		case 0x20: //DISFFC_SETACTUATORSOFF
// 			SDL_HapticPause( (*this)->haptic );
			SDL_HapticStopAll( (*this)->haptic );
			break;
	}
	return 0;
}
static STDCALL uint32_t Poll( DirectInputDevice **this )
{
	/* Only joystick */
	return 0;
}

static STDCALL uint32_t CreateDevice( void **this, const GUID *const rguid, DirectInputDevice ***directInputDevice, void *unkOuter )
{
	DirectInputDevice *dinputDev = ( DirectInputDevice * )calloc( 1, sizeof( DirectInputObject ) + sizeof( DirectInputDevice ) );
	( ( DirectInputObject * )dinputDev )->ref = 1;
	( ( DirectInputObject * )dinputDev )->is_device = 1;
	dinputDev = ( void * )dinputDev + sizeof( DirectInputObject );

	dinputDev->QueryInterface = QueryInterface;
	dinputDev->Release = Release;

	dinputDev->GetCapabilities = GetCapabilities;
	dinputDev->SetProperty = SetProperty;
	dinputDev->Acquire = Acquire;
	dinputDev->Unacquire = Unacquire;
	dinputDev->GetDeviceState = GetDeviceState;
	dinputDev->GetDeviceData = GetDeviceData;
	dinputDev->SetDataFormat = SetDataFormat;
	dinputDev->SetEventNotification = SetEventNotification;
	dinputDev->SetCooperativeLevel = SetCooperativeLevel;
	dinputDev->GetObjectInfo = GetObjectInfo;
	dinputDev->CreateEffect = CreateEffect;
	dinputDev->SendForceFeedbackCommand = SendForceFeedbackCommand;
	dinputDev->Poll = Poll;

	memcpy( &dinputDev->guid, rguid, sizeof( GUID ) );

	BOOL isOK = true;
	if ( dinputDev->guid.a == JOYSTICK && dinputDev->guid.b > 0 )
	{
		dinputDev->joy = SDL_JoystickOpen( dinputDev->guid.b - 1 );
		if ( !dinputDev->joy )
			isOK = false;
		else
			dinputDev->haptic = SDL_HapticOpenFromJoystick( dinputDev->joy ); //This doesn't work on Window$ in SDL 2.0.3...
	}
	if ( isOK && ( dinputDev->guid.a == MOUSE || dinputDev->guid.a == JOYSTICK ) )
	{
		*directInputDevice = malloc( sizeof( void * ) );
		**directInputDevice = dinputDev;
		return 0;
	}
// 	printf( "CreateDevice: error 0x%.8X\n", dinputDev->guid.a );
	free( ( void * )dinputDev - sizeof( DirectInputObject ) );
	return -1;
}
static STDCALL uint32_t EnumDevices( void **this, uint32_t devType, DIENUMDEVICESCALLBACKA callback, void *ref, uint32_t dwFlags )
{
	if ( devType == 4 /* DIDEVTYPE_JOYSTICK */ )
	{
		DIDEVICEINSTANCEA deviceInstance;
		memset( &deviceInstance, 0, sizeof deviceInstance );
		uint32_t i, n = SDL_NumJoysticks() + 1;
		for ( i = 0 ; i < n ; ++i )
		{
			if ( !mouseAsJoystick && !i )
				continue;
#ifdef WIN32
// 			printf( "%s\n", SDL_JoystickNameForIndex( i - 1 ) );
			/* To prevent joysticks duplicates - discard any xinput devices */
			if ( !strncasecmp( SDL_JoystickNameForIndex( i - 1 ), "xinput", 6 ) )
				continue;
#endif
			deviceInstance.guidInstance.a = JOYSTICK;
			deviceInstance.guidInstance.b = i;
			if ( !callback( &deviceInstance, ref ) )
				break;
		}
	}
	return 0;
}

STDCALL uint32_t DirectInputCreateA_wrap( void *hInstance, uint32_t version, DirectInput ***directInputA, void *unkOuter )
{
	DirectInput *dinput = ( DirectInput * )calloc( 1, sizeof( DirectInputObject ) + sizeof( DirectInput ) );
	( ( DirectInputObject * )dinput )->ref = 1;
	dinput = ( void * )dinput + sizeof( DirectInputObject );

	dinput->Release = Release;
	dinput->CreateDevice = CreateDevice;
	dinput->EnumDevices = EnumDevices;

	*directInputA = malloc( sizeof( void * ) );
	**directInputA = dinput;

// 	printf( "DirectInputCreateA: %p\n", **directInputA );
	return 0;
}
