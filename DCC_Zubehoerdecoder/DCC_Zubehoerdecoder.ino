/* Universeller DCC-Decoder für Weichen und (Licht-)Signale
*  Version 4.1 
 * Eigenschaften:
 * Bis zu 8 (aufeinanderfolgende) Zubehöradressen ansteuerbar
 * 1. Adresse per Programmierung einstellbar
 * 
 * 2 Ausgänge / Zubehöradresse
 * Einstellbare Funktionalität:
 *  - Servo mit Umschaltrelais zur Weichenpolarisierung
 *  - Doppelspulenantriebe
 *  - statische Ausgänge
 *  - blinkende Ausgänge
 *  - Signalfunktionen
 *  
 *  Die Funnktionalität und IO-Zuordnung wird über Tabellen im h-File festgelegt.
 *  Die Konfiguration der einzelnen Funktionen geschieht über CV-Programmierung.
 *  So sind z.B. bei Servoausgängen die Endlagen per CV-Wert einstellbar, bei Lichtsignalen ist die 
 *  Zuordnung der Ausgangszustände zum Signalzustand frei konfigurierbar.
*/
#define DCC_DECODER_VERSION_ID 0x41
// für debugging ------------------------------------------------------------
//#define DEBUG ;             // Wenn dieser Wert gesetzt ist, werden Debug Ausgaben auf dem ser. Monitor ausgegeben
//#define Serial Serial1
#include "src/Globals.h"
#include "src/FuncClasses.h"

#ifdef DEBUG
char dbgbuf[60];
#endif

//------------------------------------------ //
// die NmraDcc - Library gibt es unter https://github.com/mrrwa/NmraDcc/archive/master.zip


#ifdef __STM32F1__
    #define digitalPinToInterrupt(x) x
    #define MODISTEP    4096/6      // Grenzwerte am Analogeingang der Betriebsmodi
#else
    #define MODISTEP    1024/6
#endif
#define uint_t unsigned int


// Grenzwerte des Analogeingangs für die jeweiligen Betriebsmodi ( gesamter Bereich 0...1024):
#define ISNORMAL    MODISTEP*5          // > 853 gilt als normalbetrieb
#define ISPOM       MODISTEP*3          // <853,  >512 Allway Pom
#define ISOPEN      MODISTEP            // <512, >170 IniMode: alle Funktionsparameter beim Start initiieren
#define ISPROG      0                   // <170 Programmiermodus (Adresserkennung)

// Mögliche Funktionstypen je Ausgang. 
#define FOFF        0 // Funktionsausgang abgeschaltet
#define FSERVO      1 // Standardservoausgang 
#define FCOIL       2 // Magnetartikel
#define FSTATIC     3 // Der Ausgang wird statisch/blinkend ein bzw ausgeschaltet
#define FSIGNAL0    4 // Folgeadresse für Signale
#define FSIGNAL2    5 // 1. Signaladresse 
#define FVORSIG     6 // 1. Vorsignaladresse
#define FMAX        6  

//---------------------------------------
//Flags für iniMode:
#define AUTOADDR    1   // Automatische Addresserkennung nach Erstinitiierung oder wenn Programmiermodus aktiv
#define ROCOADDR    2   // 0: Outputadresse 4 ist Weichenadress 1
                        // 1: Outputadresse 0 ist Weichenadress 1
//-----------------------------------------
// Flags für iniFmode:
#define SAUTOOFF 0x01   // FSERVO: Impulse werden nach erreichen der Endlage abgeschaltet
#define SDIRECT  0x02   // FSERVO: Der Servo reagiert auch während der Bewegung auf einen Umschaltbefehl
#define NOPOSCHK 0x08   // FSERV: Die Ausgänge reagieren auch auf einen Befehl, wenn die aktuelle
                        // Postion nicht verändert wird.
#define LEDINVERT 0x80  // FSIGNAL: SoftledAusgänge invertieren (Bit 0 des Modebyte von FSIGNAL2/3)
//-------------------------------------------
#ifdef __STM32F1__
#include "DCC_Zubehoerdecoder-STM32.h"
//#include "TestKonf/DCC_Zubehoerdecoder-LS-STM32.h"
#else
//#include "TestKonf\DCC_Zubehoerdecoder-LS-Nano.h"
#include "DCC_Zubehoerdecoder.h"
#endif
const byte WeichenZahl = sizeof(iniTyp);

// CV Default-Werte der Standardadressen:
struct CVPair {
  uint16_t  CV;
  uint8_t   Value;
};
CVPair FactoryDefaultCVs [] =
{
  {CV_ACCESSORY_DECODER_ADDRESS_LSB, DccAddr%256},
  {CV_ACCESSORY_DECODER_ADDRESS_MSB, DccAddr/256},
  {CV_VERSION_ID, DCC_DECODER_VERSION_ID},
  /*{CV_MANUFACTURER_ID, MAN_ID_DIY},*/
  {CV_29_CONFIG, CV29_ACCESSORY_DECODER | CV29_OUTPUT_ADDRESS_MODE},
};

// allgemeine defines ---------------------------------------------------------
#define servoPins out1Pins  // Portnamen fürs Servofunktion
#define relais1Pins out2Pins
#define relais2Pins out3Pins


//-------------------------------Definition der CV-Adressen ---------------------------------------
#define CV_START    47  // Startadresse des Decoderspezifischen CV-Blocks
typedef struct {        // Definition der Struktur des decoderspezifischen CV-Blocks
    byte modeVal;       // =0x5? wenn die CV-Werte initiiert sind. Bits 0..3 für Betriebsarten
    byte PomAddrLow;      // Adresse für die POM-Programmierung
    byte PomAddrHigh;
    struct {
        byte Mode;    // Funktionalität des Ausgangs
        byte Par1;    // Servo: Position AUS ( 0...180 Grad )
                      // DoppelspuleAuto: Einschaltzeit ( 10ms-Schritte )
                      // FSTATIC:        Einschaltphase des Blinkens ( 10ms Schritte )
        byte Par2;    // Servo: Position EIN ( 0...180 Grad )
                      // DoppelspuleAuto: Einschaltzeit Ausgang 2 ( 10ms-Schritte ) 255=Dauerein
                      // FSTATIC:        Ausschaltphase des Blinkens ( 10ms Schritte )
        byte Par3;    // Servo: Speed
        byte State;   // aktueller Status des Ausgangs (Ein/Aus)
                      // Nach einem Neustart werden die Funktionsausgänge entsprechend eingestellt.
    } Fkt [WeichenZahl];
} CvVar_t;

const CvVar_t *CV = (CvVar_t *) CV_START; //Pointer auf die decoderspezifischen CV-Werte im EEProm
#define GetCvPar(ix,par) Dcc.getCV((int)&CV->Fkt[ix].par)

// ----------------------- Variable ---------------------------------------------------
byte opMode;                    // Bit 0..3 aus modeVal
byte rocoOffs;                  // 0 bei ROCO-Adressierung, 4 sonst
byte isOutputAddr;              // Flag ob Output-Adressing
word weichenAddr;               // Addresse der 1. Weiche (des gesamten Blocks)
byte dccState[WeichenZahl];     // Ausgangsstatus im DCC-Telegramm ( für FCOIL )
byte dccSoll[WeichenZahl];      // Solllage der Weichen ( wird vom DCC-Kommando gesetzt )
byte fktStatus[WeichenZahl];    // Ist-Zustand der Weichen/Signale
    // Bits 0-2
    #define GERADE  0x0             // Bit 0
    #define ABZW    0x1             // Bit 0
    #define MOVING  0x2             // Bit 1 nur für Servos,gesetzt während Umlauf
    // für Lichtsignale, aktueller Signalzustand in Bits 4-7
    #define SIG_STATE_MASK  0xf0
    #define SIG_WAIT        0x00    // Warte auf Signalbefehle
    #define SIG_CHANGED     0x10    // Signalzustand hat sich geändert, warten auf stabilen Zustand
                                    // (da die einzelnen Sollzustände sich nur nacheinander ändern können)
    #define SIG_DARK        0x20    // aktuelles Signalbild dunkelschalten ( nur 'soft' Ausgänge )
    #define SIG_NEW         0x30    // neues Signalbild aufblenden
    #define SIG_IS_DARK     0x40    // Signal ist statisch dunkelgeschaltet

// Funktionsspezifische Variable. Da pro Weichenadresse nur eine Funktion aktiv sein kann, liegen die
// funktionsspezifischen Variable in einer union übereinander
static union {
    struct { //Variable für FSERVO
        byte relaisOut;    // Ausgabewerte für die Relais ( 0/1, entspricht Sollwert )
    } ;
    struct { // Variable für FCOIL
        byte pulseON;    // Flag ob Pausentimer läuft
    } ;
    struct { // Variable für FSIGNAL
        byte soll;      // Sollstellung des Signals
        byte flags;     // Bit 1,0 : Zahl der belegten Folgeadressen (0-3)
                        // Bit 7: Signal ist dunkelgeschaltet (für Vorsignale)
        #define SIGMASK  0x03
        #define DARKMASK 0x80
    } ;
    struct { // Variable für FSTATIC
        byte ist;       // IstStellung des Ausgangs und Flag ob Blinken ist EIN
    } ; 
} fktVar[WeichenZahl];
#define fServo fktVar
#define fCoil  fktVar
#define fSig   fktVar
#define fStatic fktVar

void *Fptr[WeichenZahl];    // Pointer auf das Funktionsobjekt

//- - - Variable für Lichtsignalsteuerung - - - - - - -

#define SIG_WAIT_TIME   500     // Wartezeit in ms nach einer Änderung am Signalstatus bis die Ausgänge gesetzt werden

// Zuordnung der Softleds zu den Ausgangsports
#define PPF 3       // Outputports pro Funktion
int8_t portTyp[PPF][WeichenZahl];   // -1 = Standardport
                                  // >=0 Softled-Objekt
byte slIx;                        // Zählindex für die Vergabe der Softled-Objekte bei der Initiierung
SoftLed SigLed[ SOFTLEDS ];



byte progMode;      // Merker ob Decoder im Programmiermodus
#define NORMALMODE  0
#define ADDRMODE    1   // Warte auf 1. Telegramm zur Bestimmung der ersten Weichenadresse ( Prog-Led blinkt )
#define PROGMODE    2   // Adresse empfangen, POM-Programmierung möglich ( Prog-Led aktiv )
#define POMMODE     3   // Allways-Pom Mode ( keine Prog-Led )
#define INIMODE     4   // Wie Normalmode, aber die StandardCV-Werte und CV47-49 werden bei jedem
                        // Start aus den defaults geladen

#define SET_PROGLED digitalWrite( modePin, HIGH )
#define CLR_PROGLED digitalWrite( modePin, LOW )

// -------- Encoderauswertung ----- Justierung der Servoendlage -----------------------------
# ifdef ENCODER_AKTIV
// Die zuletzt empfangene Weichenposition kann per Encoder justiert werden. 
// Die Werte werden gespeichert, sobald eine ander Weichenposition empfangen wird.
byte adjWix;       // Weichenindex, der z.Z. vom Encoder beeinflusst wird.
byte adjPulse;      // per Encoder aktuell eingestellte Servoposition
#define NO_ADJ 255    // Wert von adjPulse solange keine Änderung erfolgt ist
#endif
bool localCV;       // lokale Änderung einers CV (Callback NotifyCV wird dann nicht ausgeführt )
//---- Library-Objekte ----
Servo8 weicheS[WeichenZahl];
EggTimer AckImpuls;
EggTimer ledTimer;  // zum Blinken der Programmierled
// Pulsetimer für Doppelspulenantriebe und Blinken
EggTimer pulseT[WeichenZahl];
NmraDcc Dcc;


//^^^^^^^^^^^^^^^^^^^^^^^^ Ende der Definitionen ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
//###########################################################################
// Ausblenden der nicht belegten (NC) Ports
#ifdef __STM32F1__
void _pinMode( byte port, WiringPinMode mode ) {
#else
void _pinMode( byte port, byte mode ) {
#endif
    if ( port != NC ) pinMode( port,  mode );
}

void _digitalWrite( byte port, byte state ) {
    if( port != NC ) digitalWrite( port, state );
}

// Bequemlichkeitsmacros:
#define getPar( Adr, Par ) Dcc.getCV((int)&CV->Fkt[Adr].Par )
//----------------------------------------------------------------------------------------
static byte getIoPin(byte wIx,byte pIx) {
    // Portadresse für Signalpins bestimmen.
    // wIx: Weichenindex (Grundadresse des Signals)
    // pIx: Signalinterner Portindex ( 0...5 )
    if ( ( pIx % PPF ) == 0 ) return out1Pins[wIx+pIx/PPF];
    else if ( ( pIx % PPF ) == 1 ) return out2Pins[wIx+pIx/PPF];
    else  return out3Pins[wIx+pIx/PPF];
}

//----------------------------------------------------------------------------------------
static void setIoPin( byte wIx, byte pIx, byte Value ) {
    // setzen/Rücksetzen eines Ausgangsport. Je nach Konfiguration wird
    // direkt oder per sofLed geschaltet
    // wIx: Weichenindex (Grundadresse des Signals)
    // pIx: Signalinterner Portindex ( 0...5 )
    // Value: HIGH oder LOW, ON oder OFF
    
    // Falls es ein Port der Folgeadresse ist (Signal!)
    wIx += ( pIx / PPF );
    pIx = pIx % PPF;
   //DB_PRINT( "set port Typ %d, pIx %d,  wIx %d, pin  %d, Wert=%d ", portTyp[pIx][wIx], pIx, wIx, getIoPin(wIx,pIx), Value ); 
    if ( portTyp[pIx][wIx] >=0 ) {
        // Es ist ein Softled-Ausgang
        byte typ = Value? LINEAR : BULB;
        SigLed[ portTyp[pIx][wIx] ].write( Value, typ );
    } else {
        // Standard-Digitalausgang
        if ( pIx == 0 ) { _digitalWrite( out1Pins[wIx], Value ); }
        else if ( pIx == 1 ) { _digitalWrite( out2Pins[wIx], Value ); } 
        else { _digitalWrite( out3Pins[wIx], Value ); }
    }
}

//----------------------------------------------------------------------------------------
byte getSigMask( byte sIx, byte sState ) {
    // Ausgangsmaske für Zustand sState am Signal sIx bestimmen
    // sIx; Grundindex des Signals
    // sState: Signalzustand
    static int CVBaseAdr[] = { 51,52,56,57,61,62,66,67 } ; // schon für 4 Adressen vorgesehen
    byte CVoffs = sIx*5;
    return Dcc.getCV( CVBaseAdr[sState] + CVoffs );
}

//----------------------------------------------------------------------------------------
static void setSignal ( byte wIx ) {
    // alle einem Signal zugeordneten Ausgänge entsprechend dem derzeitigen Signalzustand setzen
    // wIx: Grundindex des Signals
    //byte sigZustand; // aktueller Signalzustand, abgeleitet aus den Weichenzuständen
    byte sigOutMsk;  // Bitmaske der Ausgangsports (Bit=1:Ausgang setzen, Bit=0 Ausgang rücksetzen
                     /* Diese Maske steht für jeden Signalzustand in entsprechenden CV-Paramtern:
                     *  CV51+offs    Bitmuster der Ausgänge für Befehl 1.Adresse 0 (rot)
                     *  CV52+offs    Bitmuster der Ausgänge für Befehl 1.Adresse 1 (grün)
                     *  CV56+offs    Bitmuster der Ausgänge für Befehl 2.Adresse 0 (rot)
                     *  CV57+offs    Bitmuster der Ausgänge für Befehl 2.Adresse 1 (grün)
                     *  die folgenden CV's sind nur relevant bei FSIGNAL3 (3 Adressen, 8 Zustände 6 Ausgänge)
                     *  CV61+offs    Bitmuster der Ausgänge für Befehl 3.Adresse 0 (rot)
                     *  CV62+offs    Bitmuster der Ausgänge für Befehl 3.Adresse 1 (grün)
                     *  offs= wIx*5
                     */
    DB_PRINT( "Sig %d EIN (%d)", wIx, fSig[wIx].soll );
    switch ( iniTyp[wIx] ) {
      case FVORSIG:
      case FSIGNAL2:
        // der Sollzustand des gesamten Signals steht in fSig[wIx].soll)
        // Ausgangszustände entsprechend Signalzustand bestimmen (CV-Wert)
        sigOutMsk = getSigMask( wIx, fSig[wIx].soll ) ;
        for ( byte i=0; i< PPF*((fSig[wIx].flags & 0x3) + 1) ; i++ ) {
            setIoPin( wIx, i, sigOutMsk&1 );
            sigOutMsk = sigOutMsk >> 1;
        }
        break;
      default:
        // sollte hier nie hinkommen, keine Reaktion wenn kein Signaltyp
        break;
    }
    //DB_PRINT( " Signal %d, Status=0x%02x, Ausgänge: 0x%02x ", wIx, sigZustand, Dcc.getCV( CVBaseAdr[sigZustand] + CVoffs)  );
}
//----------------------------------------------------------------------------------------
static void clrSignal ( byte wIx ) {
    // alle 'Soft'Leds des Signals ausschalten
    // wIx: Grundindex des Signals
    switch ( iniTyp[wIx] ) {
      case FVORSIG:
      case FSIGNAL2:
        DB_PRINT( "Sig %d AUS", wIx );
        // nur 'soft' Ausgangszustände löschen
        for ( byte pIx=0; pIx< PPF*((fSig[wIx].flags & 0x3) + 1) ; pIx++ ) {
            if ( portTyp[pIx%PPF][wIx+(pIx/PPF)] >=0 ) {
                // Es ist ein Softled-Ausgang
                setIoPin( wIx, pIx, OFF );
            }
        }
        break;
      default:
        // sollte hier nie hinkommen, keine Reaktion wenn kein Signaltyp
        break;
    }
}
//----------------------------------------------------------------------------------------

void setup() {

#ifdef __STM32F1__
   disableDebugPorts();     // JTAG und SW Ports freigeben
#endif
    #ifdef FIXMODE
    progMode = FIXMODE;
    int temp = -1;
    #else
    // Betriebsart auslesen
    int temp = analogRead( betrModeP );
    if ( temp > ISNORMAL ) {
        // Normalbetrieb
        progMode = NORMALMODE;
    } else if ( temp > ISPOM ) {
        // PoM immer aktiv 
        progMode = POMMODE;
    } else if ( temp > ISOPEN ) {
        // IniMode - GrundCV's werden immer initiiert
        progMode = INIMODE;
    } else {
        // Programmiermodus, automatische Adresserkennung
        progMode = ADDRMODE;
    }
    #endif
    
    _pinMode( modePin, OUTPUT );
    
    #ifdef DEBUG
    Serial.begin(115200); //Debugging
        #ifdef __STM32F1__
        // auf STM32: warten bis USB aktiv (maximal 6sec)
        {  unsigned long wait=millis()+6000;
           while ( !Serial && (millis()<wait) );
        }
        #endif
    #endif
    #ifdef DEBUG
     Serial.print( "Betr:" ); Serial.print(temp);Serial.print(" -> Mode=" );
      switch ( progMode ) {
        case NORMALMODE:
          Serial.println( "Std" );
          break;
        case ADDRMODE:
          Serial.println( "Addr" );
          break;
        case POMMODE:
          Serial.println( "Pom" );
          break;
        case INIMODE:
          Serial.println( "Ini" );
          break;
        default:
          Serial.println( "??" );
          
      }
    #endif

    _pinMode( ackPin, OUTPUT );
    //-----------------------------------
    // nmra-Dcc Lib initiieren
    Dcc.pin( digitalPinToInterrupt(dccPin), dccPin, 1); 
    if ( progMode == NORMALMODE || progMode == INIMODE ) {
        // keine POM-Programmierung
        Dcc.init( MAN_ID_DIY, DCC_DECODER_VERSION_ID, FLAGS_DCC_ACCESSORY_DECODER, (uint8_t)((uint16_t) 0) );
        CLR_PROGLED;
    } else {
        // POM Programmierung aktiv
        Dcc.init( MAN_ID_DIY, DCC_DECODER_VERSION_ID, FLAGS_DCC_ACCESSORY_DECODER, (uint8_t)((uint_t) &CV->PomAddrLow) );
        SET_PROGLED;
    }

    //-------------------------------------
    // CV's initiieren
    if ( (Dcc.getCV( (int) &CV->modeVal)&0xf0) != ( iniMode&0xf0 ) || analogRead(resModeP) < 100 ) {
        // In modeVal steht kein sinnvoller Wert ( oder resModeP ist auf 0 ),
        // alles initiieren mit den Defaultwerten
        // Wird über DCC ein 'factory-Reset' empfangen wird modeVal zurückgesetzt, was beim nächsten
        // Start zum initiieren führt.
        //
        // Standard-CV's
        for ( byte i=0; i<(sizeof(FactoryDefaultCVs) / sizeof(CVPair)); i++ ) {
                Dcc.setCV( FactoryDefaultCVs[i].CV, FactoryDefaultCVs[i].Value);
        }
        // Decoderspezifische CV's
        Dcc.setCV( (int) &CV->PomAddrLow, PomAddr%256 );
        Dcc.setCV( (int) &CV->PomAddrHigh, PomAddr/256 );
        Dcc.setCV( (int) &CV->modeVal, iniMode );
        for ( byte i = 0; i<WeichenZahl; i++ ) {
            Dcc.setCV( (int) &CV->Fkt[i].Mode, iniFmode[i] );
            Dcc.setCV( (int) &CV->Fkt[i].Par1, iniPar1[i] );
            Dcc.setCV( (int) &CV->Fkt[i].Par2, iniPar2[i] );
            Dcc.setCV( (int) &CV->Fkt[i].Par3, iniPar3[i] );
            Dcc.setCV( (int) &CV->Fkt[i].State, iniPar4[i] );
        }
    } else if ( progMode == INIMODE ) {
        // Standard-CV's immer initiieren
        for ( byte i=0; i<(sizeof(FactoryDefaultCVs) / sizeof(CVPair)); i++ ) {
                Dcc.setCV( FactoryDefaultCVs[i].CV, FactoryDefaultCVs[i].Value);
        }
        Dcc.setCV( (int) &CV->PomAddrLow, PomAddr%256 );
        Dcc.setCV( (int) &CV->PomAddrHigh, PomAddr/256 );
        Dcc.setCV( (int) &CV->modeVal, iniMode );
        
        // Funktionsspezifische Parameter aus INI-Tabelle laden
        for ( byte i = 0; i<WeichenZahl; i++ ) {
            Dcc.setCV( (int) &CV->Fkt[i].Mode, iniFmode[i] );
            Dcc.setCV( (int) &CV->Fkt[i].Par1, iniPar1[i] );
            Dcc.setCV( (int) &CV->Fkt[i].Par2, iniPar2[i] );
            Dcc.setCV( (int) &CV->Fkt[i].Par3, iniPar3[i] );
            switch ( iniTyp[i] ) {
              case FSIGNAL2:
              case FVORSIG:
              case FSIGNAL0:
                // bei den Signaltypen auch den 5. CV-Wert als Parameter laden
                Dcc.setCV( (int) &CV->Fkt[i].State, iniPar4[i] );
                break;
              default:
                ;
            }
        }
        
    }
    
    // Betriebsart auslesen
    opMode = Dcc.getCV( (int) &CV->modeVal) &0x0f;
    rocoOffs = ( opMode & ROCOADDR ) ? 4 : 0;
    

    // Encoder-Init
    IniEncoder();

    //--- Ende Grundinitiierung ---------------------------------

    setWeichenAddr(); // 1. Weichenadresse berechnen
        
    DBprintCV(); // im Debug-Mode alle CV-Werte ausgeben
    
    for ( byte wIx=0; wIx<WeichenZahl; wIx++ ) {
        // Funktionen initiieren
        fktStatus[wIx] = Dcc.getCV( (int) &CV->Fkt[wIx].State );
        dccSoll[wIx] = fktStatus[wIx];
        portTyp[0][wIx] = -1; // Standardport ist digital IO (kein Softled)
        portTyp[1][wIx] = -1;
        portTyp[2][wIx] = -1;
        switch (iniTyp[wIx] )  {
          case FSERVO:
            if ( servoPins[wIx] != NC ) {
            weicheS[wIx].attach( servoPins[wIx], Dcc.getCV(  (int) &CV->Fkt[wIx].Mode) & SAUTOOFF );
            weicheS[wIx].setSpeed( Dcc.getCV(  (int) &CV->Fkt[wIx].Par3 ) );
            }
            _pinMode( relais1Pins[wIx], OUTPUT );
            _pinMode( relais2Pins[wIx], OUTPUT );
            // Servowerte und Relaisausgang initiieren und ausgeben
            if ( dccSoll[wIx] == GERADE ) {
                weicheS[wIx].write( Dcc.getCV( (int) &CV->Fkt[wIx].Par1 ) );
            } else {
                weicheS[wIx].write( Dcc.getCV( (int) &CV->Fkt[wIx].Par2 ) );
            }
            fktStatus[wIx] = dccSoll[wIx];
            fServo[wIx].relaisOut = fktStatus[wIx];
            _digitalWrite( relais1Pins[wIx], fServo[wIx].relaisOut );
            _digitalWrite( relais2Pins[wIx], !fServo[wIx].relaisOut );
            break;
          case FCOIL:
            Fptr[wIx] = new Fcoil( ( int ) &CV->Fkt[wIx].Mode , out1Pins[wIx], out2Pins[wIx] );
            break;
          case FSTATIC:
            Fptr[wIx] = new Fstatic( ( int ) &CV->Fkt[wIx].Mode , out1Pins[wIx], out2Pins[wIx] );
            break;
          case FSIGNAL2:
          case FVORSIG: {
            // Signaldecoder mit 2 oder 3 Adressen
            // Bei Signalen steht Gesamtzustand des Signals in fSig[i].soll
            // in dccSoll[..] stehen die einzelnen empfangenen Dcc-Sollzustände der Weichenadressen
            // Signale werden immer mit dem Grundzustand initiiert ( = HP0 oder Hp00 )
            fSig[wIx].soll = 0;
            
            // Bestimmen der Zahl der Folgeadressen
            fSig[wIx].flags &= DARKMASK;   // Dunkelschaltbit kann bereits gesetzt sein, nicht löschen
            for( byte i=1; iniTyp[wIx+i] == FSIGNAL0 ; i++  ) { fSig[wIx].flags++; }
            // Zahl der zugeordneten Ausgangsports (maximal 8 genutzt)
            byte outMax = min( 8, PPF * (fSig[wIx].flags + 1) );   
            DB_PRINT( "Sig %d, flags=0x%02x, soll %d", wIx, fSig[wIx].flags, fSig[wIx].soll );
            
            // Ist- und Sollzustände, Modi der Ausgänge setzen
            for ( byte i=0; i<= (fSig[wIx].flags&SIGMASK) ; i++ ) {
                fktStatus[wIx+i] = 0;   // Istzustände
                dccSoll[wIx+i] = 2;     // EinzelSollzustände auf 'invalid'
                
                // Modi der Ausgänge setzen
                byte sigMode = getPar(wIx+i , Mode); // Bitcodierung harte/weiche Ledumschaltung
                for ( byte sigO = 0; sigO < 3 ; sigO++ ) {
                    // sigMode enthält bitcodiert die Info ob harte/weiche Umschaltung
                    byte outPin = getIoPin( wIx+i, sigO );
                   //DB_PRINT( "SigMode=%02x, Index= %d, pin=%d, ", sigMode, sigO,outPin);
                    portTyp[sigO][wIx+i] = -1; // Default ( auch für 'NC' Ports )
                    if ( sigMode & (1<<sigO) ) {
                        // Bit gesetzt -> harte Umschaltung
                        _pinMode(outPin, OUTPUT );
                    } else {
                        // Bit = 0 -> Softled
                        if ( outPin != NC ) {
                            byte att, rise, writ;
                            att=SigLed[slIx].attach( outPin , getPar(wIx,Mode)&LEDINVERT );
                            SigLed[slIx].riseTime( SIG_RISETIME );
                            SigLed[slIx].write( OFF, BULB );
                            portTyp[sigO][wIx+i] = slIx++;
                           //DB_PRINT( "Softled, pin %d, Att=%d", outPin, att );
                        }
                    }
                    //DB_PRINT( "portTyp[%d][%d] = %d" , sigO&1, wIx+(sigO>>1), portTyp[sigO&1][wIx+(sigO>>1)] );
                }
            } // Ende for über die Signal-Subadressen
            
            DB_PRINT( "Sig %d, flags=0x%02x, soll %d", wIx, fSig[wIx].flags, fSig[wIx].soll );
            setSignal(wIx); // Signalausgänge setzen
            // prüfen ob Vorsignal dunkelgeschaltet werden muss
            if ( iniTyp[wIx] == FSIGNAL2  ) {
                byte vsIx = getPar( wIx, Par3 );
                DB_PRINT( "Vorsig=%d", vsIx );
                if ( vsIx > 0 && vsIx <= WeichenZahl && iniTyp[vsIx-1] == FVORSIG ) {
                    // gültiger Verweis auf ein Vorsignal, Dunkelschaltflag setzen
                    fSig[vsIx-1].flags |= DARKMASK;
                }
            }
            // Folgeadressen bei der Initiierung überspringen
            //DB_PRINT( "Ini: wIx=%d, flags=%d", wIx, fSig[wIx].flags );
            wIx += (fSig[wIx].flags & 0x3);
            }
            break;
          default: // auch FSIGNAL0
            // hier werden gegebenenfalls Signalfolgetypen übersprungen
            ;
        } // Ende switch Funktionstypen
    } // Ende loop über alle Funktionen
    DB_PRINT(">>> Setup-Ende >>>>",0);
}



////////////////////////////////////////////////////////////////
void loop() {
    //if (digitalRead( A4)) _digitalWrite(A4,LOW); else _digitalWrite(A4,HIGH); // Test Zykluszeit
    getEncoder();    // Drehencoder auswerten und Servolage gegebenenfalls anpassen
    
    Dcc.process(); // Hier werden die empfangenen Telegramme analysiert und der Sollwert gesetzt
    #ifdef DEBUG
    // Merker CV für CV-Ausgabe rücksetzen (MerkerCV ist 1.CV hinter dem CV-Block für die ausgangskonfiguration)
    Dcc.setCV( (int) &CV->Fkt[WeichenZahl].Mode , 0xff );
    #endif
    
    // Ausgänge ansteuern
    for ( byte i=0; i<WeichenZahl; i++ ) {
        switch ( iniTyp[i]  ) {
          case FSERVO: // Servoausgänge ansteuern ----------------------------------------------
            if ( fktStatus[i] & MOVING ) {
                // Weiche wird gerade ungestellt, Schaltpunkt Relais und Bewegungsende überwachen
                if ( dccSoll[i] != (fktStatus[i] & 0x01) && (getPar(i,Mode) & SDIRECT) ) {
                    // Es wurde die Servoposition umgeschalten und das Flag SDIRECT ist
                    // gesetzt: Bewegung abbrechen und Moving-Bit löschen.
                    // Im nächsten loop-Durchlauf wird dann auf die neue Position reagiert
                    weicheS[i].write( weicheS[i].read() );
                    fktStatus[i] &= 0x1; 
                }
                if ( weicheS[i].moving() < 50 ) fServo[i].relaisOut = fktStatus[i]& 0x1;
                if ( weicheS[i].moving() == 0 ) {
                    // Bewegung abgeschlossen, 'MOVING'-Bit löschen und Lage in CV speichern
                    fktStatus[i] &= 0x1; 
                    Dcc.setCV( (int) &CV->Fkt[i].State, fktStatus[i] );
                    if ( getPar(i,Mode) & NOPOSCHK ) {
                        // Soll auf 'ungültig' stellen, damit auch neue Telegramme mit gleicher
                        // Position erkannt werden (ausser es wurde schon verändert )
                        if ( dccSoll[i] == fktStatus[i] ) dccSoll[i] = SOLL_INVALID;
                        DB_PRINT( "dccSoll=%d", dccSoll[i] );
                    }
                }
            } else if ( dccSoll[i] != SOLL_INVALID  && (dccSoll[i] != fktStatus[i] || (getPar(i,Mode) & NOPOSCHK))  ) {
                // Weiche muss umgestellt werden
                DB_PRINT( "WeicheIx=%d stellen, Ist=%d,Soll=%d", i, fktStatus[i], dccSoll[i] );
                fktStatus[i] = dccSoll[i] | MOVING; // Istwert auf Sollwert und MOVING-Bit setzen.
                if ( dccSoll[i] == GERADE ) {
                    weicheS[i].write( Dcc.getCV( (int) &CV->Fkt[i].Par1 ) );
                } else {
                    weicheS[i].write( Dcc.getCV( (int) &CV->Fkt[i].Par2 ) );
                }
            }
            // Relaisausgänge setzen
            if ( relais2Pins[i] == NC ) {
                // Variante mit einem Relais, wird in Bewegungsmitte umgeschaltet
                _digitalWrite( relais1Pins[i], fServo[i].relaisOut );
            } else {
                // Variante mit 2 Relais, während der Bewegung beide Relais abschalten
                if ( fktStatus[i] & MOVING ) {
                    _digitalWrite( relais1Pins[i], OFF );
                    _digitalWrite( relais2Pins[i], OFF );
                } else {
                    // im Stillstand des Servos entsprechend relaisout schalten
                    _digitalWrite( relais1Pins[i], fServo[i].relaisOut );
                    _digitalWrite( relais2Pins[i], !fServo[i].relaisOut );
                }
            }
            
            break;

          case FCOIL: //Doppelspulenantriebe ------------------------------------------------------
           //if ( dccSoll[i] != SOLL_INVALID ) DB_PRINT( "SollCoil=%d", dccSoll[i] );
           ( ( Fcoil * )Fptr[i])->chkState(&dccSoll[i],dccState[i]);
            break;
          case FSTATIC: // Ausgang statisch ein/ausschalten ------------------------------------
           ( ( Fstatic * )Fptr[i])->chkState(dccSoll[i]);
            break;
          case FVORSIG:
          case FSIGNAL2:
            // Sollzustand des gesamten Signals bestimmen ( wird in fSig[i].soll gespeichert )
            // Damit Weichenbefehle immer erkannt werden, werden die Weichensollwerte auf einen
            // 'ungültig'-Wert gesetzt nachdem sie in fSig[i].soll integriert wurden
            
            for ( byte j=0; j<= (fSig[i].flags&SIGMASK) ; j++ ) {
                // Schleife über die Signal-Sub-Adressen
                if ( dccSoll[i+j] < 2 ) { 
                    fSig[i].soll = dccSoll[i+j]+2*j; dccSoll[i+j] = 2; 
                }
            }

             
            switch ( fktStatus[i] & SIG_STATE_MASK ) {
              case SIG_WAIT:  
                // warten auf Zustandsänderung am Signal
               if ( ( fktStatus[i] & ~SIG_STATE_MASK ) != fSig[i].soll ) {
                    // Sollzustand hat sich verändert, püfen ob erlaubter Zustand
                    if (  getSigMask( i,  fSig[i].soll) == 0xff )  {
                        // Sollzustand hat Signalmaske 0xff -> diesen Zustand ignorieren
                        // Sollzustand auf Istzustand zurücksetzen
                        fSig[i].soll = fktStatus[i] & ~SIG_STATE_MASK ;
                    } else {
                        // Gültiger Zustand, übernehmen, Flag setzen und Timer aufziehen
                        fktStatus[i] = fSig[i].soll | SIG_DARK;
                        pulseT[i].setTime( SIG_WAIT_TIME ) ;
                       DB_PRINT( "Sig %d neu ist=0x%02x ", i, fktStatus[i] );
                        // Dunkelschaltung am Vorsignal setzen
                        if ( iniTyp[i] == FSIGNAL2  ) {
                            byte vsIx = getPar( i, Par3 );
                           DB_PRINT( "Vorsig=%d", vsIx );
                            if ( vsIx > 0 && vsIx <= WeichenZahl && iniTyp[vsIx-1] == FVORSIG ) {
                                // gültiger Verweis auf ein Vorsignal, Dunkelschaltflag bestimmen
                                byte darkStates = getPar( i, State );
                                if ( darkStates & ( 1<< fSig[i].soll) ) {
                                    fSig[vsIx-1].flags |= DARKMASK;
                                } else {
                                    fSig[vsIx-1].flags &= ~DARKMASK;
                                }
                               DB_PRINT( "darkStates=0x%02x, VorsigFlags= 0x%02x" , darkStates, fSig[vsIx-1].flags );
                            }
                         }
                    }
                } else if ( fSig[i].flags & DARKMASK ) {
                    // Signal dunkelschalten
                    DB_PRINT( "DARK-Start Ix=%d" , i );
                    clrSignal(i);
                    fktStatus[i] &= ~SIG_STATE_MASK; 
                    fktStatus[i] |= SIG_IS_DARK;
                }
                break;
              case SIG_IS_DARK:
                // Signal ist dauerhaft dunkelgeschaltet, warten bis DARK-Flag gelöscht wird.
                //DB_PRINT( "SIG_IS_DARK Ix=%d" , i );
                 if ( ! (fSig[i].flags & DARKMASK) ) {
                    DB_PRINT( "DARK-End Ix=%d" , i );
                    fktStatus[i] = fSig[i].soll+1;  // ist != soll -> Signalbild wird aufgeschaltet
                    fktStatus[i] |= SIG_WAIT;
                }
                break;
              case SIG_DARK:
                // wenn Timer abgelaufen, Signalbild dunkelschalten
                if ( ! pulseT[i].running() ) {
                    // ist abgelaufen: Soft-Ausgänge zurücksetzen
                    clrSignal(i); // Signalbild dunkelschalten
                    pulseT[i].setTime( SIG_DARK_TIME );
                    fktStatus[i] &= ~SIG_STATE_MASK; 
                    fktStatus[i] |= SIG_NEW;
                }
                break;
              case SIG_NEW:
                // Wenn Timer abgelaufen, neues Signalbild aufschalten
                if ( ! pulseT[i].running() ) {
                    // ist abgelaufen: neues Signalbild
                    fktStatus[i] = SIG_WAIT | fSig[i].soll; // = aktueller sollwert, der gesetzt wird
                    setSignal(i); // Signalbild einschalten
                }
                break;
                default:
                ;
            }
           break;
          case FSIGNAL0:
            // Signalfolgetypen überspringen
            ;
        } // - Ende Switch Funktionstypen-------------------------------------------
    } // Ende Schleife über die Funktionen (Weichen)---------------



    // Ackimpuls abschalten--------------------------
    if ( !AckImpuls.running() ) _digitalWrite( ackPin, LOW );

    // Programmierled blinkt im Programmiermode bis zum Empfang einer Adresse
    if ( ! ledTimer.running() && progMode == ADDRMODE) {
        ledTimer.setTime( 500 );
        if ( digitalRead( modePin ) ) 
            CLR_PROGLED;
        else
            SET_PROGLED;
    }
}
//////////////////////////////////////////////////////////////
// Unterprogramme, die von der DCC Library aufgerufen werden:
//------------------------------------------------
// Die folgende Funktion wird von Dcc.process() aufgerufen, wenn ein Weichentelegramm empfangen wurde
void notifyDccAccState( uint16_t Addr, uint16_t BoardAddr, uint8_t OutputAddr, uint8_t State ){
    // Weichenadresse berechnen
    byte i;
    word wAddr = Addr+rocoOffs; // Roco zählt ab 0, alle anderen lassen die ersten 4 Weichenadressen frei
    // Im Programmiermodus bestimmt das erste empfangene Programm die erste Weichenadresse
    if ( progMode == ADDRMODE ) {
        // Adresse berechnen und speichern
        if (isOutputAddr ) {
            weichenAddr = wAddr;
            Dcc.setCV( CV_ACCESSORY_DECODER_ADDRESS_LSB, wAddr%256 );
            Dcc.setCV( CV_ACCESSORY_DECODER_ADDRESS_MSB, wAddr/256 );
        } else {
            Dcc.setCV( CV_ACCESSORY_DECODER_ADDRESS_LSB, BoardAddr%64 );
            Dcc.setCV( CV_ACCESSORY_DECODER_ADDRESS_MSB, BoardAddr/64 );
            weichenAddr = (Dcc.getAddr( )-1)*4 +1 + rocoOffs;
        }
        progMode = PROGMODE;
        SET_PROGLED;
       //DB_PRINT( "Neu: Boardaddr: %d, 1.Weichenaddr: %d", BoardAddr, weichenAddr );
    }
    // Testen ob eigene Weichenadresse
    //DB_PRINT( "DecAddr=%d, Weichenadresse: %d , Ausgang: %d, State: %d", BoardAddr, wAddr, OutputAddr, State );
    // Prüfen ob Adresse im Decoderbereich
    if ( wAddr >= weichenAddr && wAddr < (weichenAddr + WeichenZahl) ) {
        // ist eigene Adresse, Sollwert setzen
        byte Ix = wAddr-weichenAddr;
        dccSoll[Ix] =  OutputAddr & 0x1;
        dccState[Ix] = State;
        //DB_PRINT( "Weiche %d, Index %d, Soll %d, Ist %d", wAddr, Ix, dccSoll[Ix],  fktStatus[Ix] );
        ChkAdjEncode( Ix );
    }
    // Prüfen ob Vorsignal über Hauptsignaladresse geschaltet werden muss
    for ( i = 0; i < WeichenZahl; i++ ) {
        int vsAdr;
        if ( iniTyp[i] == FVORSIG  ) {
            // Adresse des zugehörigen Hauptsignals bestimmen
            vsAdr = getPar(i, Par3) + 256 * getPar(i, State);
            if ( vsAdr == wAddr ) {
                dccSoll[i] =  OutputAddr & 0x1;
               DB_PRINT( "Vorsig0 %d, Index %d, Soll %d, Ist %d", wAddr, i, dccSoll[i],  fktStatus[i] );
                break; // Schleifendurchlauf abbrechen, es kann nur eine Signaladresse sein
            } else {
                // Folgeadressen ( bei mehrbegriffigen Vorsignalen ) prüfen
                while( i+1 < WeichenZahl && iniTyp[i+1] == FSIGNAL0 ) {
                    // Folgeadresse vergleichen
                    i++;
                    if ( ++vsAdr == wAddr ) {
                        dccSoll[i] =  OutputAddr & 0x1;
                        DB_PRINT( "Vorsig1 %d, Index %d, Soll %d, Ist %d", wAddr, i, dccSoll[i],  fktStatus[i] );
                    }
                }
            }
        }
    }
}
//---------------------------------------------------
// wird aufgerufen, wenn die Zentrale ein CV ausliest. Es wird ein 60mA Stromimpuls erzeugt
void notifyCVAck ( void ) {
    // Ack-Impuls starten
    //DB_PRINT( "Ack-Pulse" );
    AckImpuls.setTime( 6 );
    _digitalWrite( ackPin, HIGH );
}
//-----------------------------------------------------
// Wird aufgerufen, nachdem ein CV-Wert verändert wurde
void notifyCVChange( uint16_t CvAddr, uint8_t Value ) {
    if ( !localCV ) {
        //CV wurde über nmraDCC geändert. Ist dies eine aktive Servoposition, dann die Servoposition
        // entsprechend anpassen
       //DB_PRINT( "neu: CV%d=%d", CvAddr, Value );
        for ( byte i=0; i<WeichenZahl; i++ ) {
            // prüfen ob Ausgang einen Servo ansteuert:
            switch ( iniTyp[i] ) {
              case FSERVO:
                // gehört der veränderte CV zu diesem Servo?
                if (  (CvAddr == (uint_t) &CV->Fkt[i].Par1 && dccSoll[i] == GERADE) ||
                      (CvAddr == (uint_t) &CV->Fkt[i].Par2 && dccSoll[i] == ABZW ) ){
                    // Es handelt sich um die aktuelle Position des Servos,
                    // Servo neu positionieren
                    //DB_PRINT( "Ausg.%d , Pos. %d neu einstellen", i, dccSoll[i] );
                     weicheS[i].write( Value );
                } else if ( CvAddr == (uint_t) &CV->Fkt[i].Par1 ||
                            CvAddr == (uint_t) &CV->Fkt[i].Par2  ) {
                      // ist nicht de aktuelle Position des Servos, Servo umstellen
                      dccSoll[i] = ! dccSoll[i];
                } else if ( CvAddr == (uint_t) &CV->Fkt[i].Par3 ) {
                    // die Geschwindigkeit des Servo wurde verändert
                    //DB_PRINT( "Ausg.%d , Speed. %d neu einstellen", i, Value );
                    weicheS[i].setSpeed( Value );
                }
            }
        }
        #ifdef DEBUG
            // prüfen ob die CV-Adresse HINTER den Weichenadressen verändert wurde. Wenn ja,
            // alle CV-Werte ausgeben und Wert wieder auf 0xff setzen
            if ( CvAddr ==  (int) &CV->Fkt[WeichenZahl].Mode && Value !=0xff ) {
                DBprintCV();
            }
        #endif
    
        // prüfen ob die Weichenadresse verändert wurde. Dies kann durch Ändern der
        // Adressierungsart in CV29 oder direkt durch Ändern der Decoderadresse geschehen.
        // Wird die Decoderadresse geändert muss zuerst das MSB (CV9) verändert werden. Mit dem
        // Ändern des LSB (CV1) wird dann die Weichenadresse neu berechnet
        if ( CvAddr ==  29 || CvAddr ==  1 ) setWeichenAddr();
    
    }
}    
//-----------------------------------------------------
void notifyCVResetFactoryDefault(void) {
    // Auf Standardwerte zurücksetzen und Neustart
    Dcc.setCV( (int) &CV->modeVal, 255 );
    delay( 20 );
    DB_PRINT( "Reset: modeVal=0x%2x", Dcc.getCV( (int) &CV->modeVal ) );
    delay(500);
    softReset();
}
//------------------------------------------------------
#ifdef DEBUG
void notifyDccReset( uint8_t hardReset ) {
    //if ( hardReset > 0 )//DB_PRINT("Reset empfangen, Value: %d", hardReset);
    // wird bei CV-Auslesen gesendet
}
#endif
//--------------------------------------------------------

/////////////////////////////////////////////////////////////////////////
// Unterprogramme zur Servojustierung mit Drehencoder
void IniEncoder( void ) {
    #ifdef ENCODER_AKTIV
    // Encoder initiieren
    _pinMode( encode1P, INPUT_PULLUP );
    _pinMode( encode2P, INPUT_PULLUP );
    adjWix = WeichenZahl;
    adjPulse = NO_ADJ;
    #endif
}
   
void getEncoder(  ) {
    #ifdef ENCODER_AKTIV
    
    static bool dirState, taktState;
    static int lastCnt,jogCount = 0;
    static enum {IDLE0, TAKTHIGH, TAKTLOW } encoderState;
    // Encoder-Statemachine
    switch ( encoderState ) {
      case IDLE0: // Grundstellung, warten auf Statuswechsel an encode1P
        if ( digitalRead( encode1P )!= dirState ) {
            bool temp = digitalRead( encode2P );
            if (  temp != taktState ) {
                // Es gab eine zusätzliche Taktflanke, Richtungsumkehr!
                //Serial.println( "++++Doppeltakt" );
                jogCount = lastCnt;
            }
            // Taktinput wieder scharf
            encoderState = digitalRead( encode2P )? TAKTHIGH : TAKTLOW;
        }
        break;
      case TAKTHIGH:
        // warte auf neg. Flanke am Taktinput
        if ( ! digitalRead( encode2P )  ) {
            // Bei Schaltern mit doppelter Rastung auch hier Puls erzeugen
            taktState = LOW;
            dirState = digitalRead( encode1P );
            lastCnt = jogCount;
            # ifdef ENCODER_DOUBLE
            if ( dirState )
                jogCount++;
            else jogCount--;
            #endif
            encoderState = IDLE0;
        } 
        break;
      case TAKTLOW:
        // warte auf pos. Flanke am Taktinput
        if ( digitalRead( encode2P )  ) {
            taktState = HIGH;
            // Richtung bestimmen
            dirState = digitalRead( encode1P );
            lastCnt = jogCount;
            if ( dirState )
                jogCount--;
            else jogCount++;
            encoderState = IDLE0;
        } 
    }
    #ifdef DEBUG
    // encoderzähler ausgeben
    if ( jogCount != 0 ) {
        DB_PRINT( "Encoder: %d", jogCount );        
    }
    #endif

    if ( adjWix < WeichenZahl && !(fktStatus[adjWix] & MOVING ) ) {
        // es gibt eine aktuell zu justierende Weiche, die sich nicht
        // gerade bewegt
        if ( jogCount != 0 ) {
            // Drehencoder wurde bewegt 
            if ( adjPulse == NO_ADJ ) {
                // ist erster Justierimpuls, aktuelle Position aus CV auslesen
                if ( dccSoll[adjWix] == GERADE ) {
                    adjPulse = Dcc.getCV( (int) &CV->Fkt[adjWix].Par1 );
                } else {
                    adjPulse = Dcc.getCV( (int) &CV->Fkt[adjWix].Par2 );
                }
            }
            if ( (jogCount>0 && adjPulse<180) || (jogCount<0 && adjPulse>0) )
                adjPulse += jogCount; // adjPulse nur im Bereich 0...180 
            weicheS[adjWix].write( adjPulse );
        } else if ( analogRead( resModeP ) < 500 ) {
            // Mittelstellungstaster gedrückt
            weicheS[adjWix].write( 90 );
        }
    }
    jogCount = 0;
    #endif
}

void ChkAdjEncode( byte WIndex ){
    #ifdef ENCODER_AKTIV
    // nach dem Empfang einer Weichenadresse wird geprüft, ob eine vorherige Justierung gespeichert werden
    // muss. Die empfangene Weichenadresse wird als neue Justieradresse gespeichert wenn es sich um einen
    // Servoantrieb handelt.
    if ( adjPulse != NO_ADJ ) {
        // Es wurde justiert, testen ob gespeichert werden muss (Weichenwechsel)
        if ( WIndex != adjWix || fktStatus[adjWix] != dccSoll[adjWix] ) {
            // Weiche wurde umgeschaltet, oder eine andere Weiche betätigt -> Justierung speichern
            localCV = true; // keine Bearbeitung in Callback-Routine NotifyCvChange
            if ( fktStatus[adjWix] == GERADE ) {
                Dcc.setCV( (int) &CV->Fkt[adjWix].Par1, adjPulse );
            } else {
                Dcc.setCV( (int) &CV->Fkt[adjWix].Par2, adjPulse );
            }
            adjPulse = NO_ADJ;
            localCV = false;
        }
    }
    adjWix = WIndex;
    if ( adjWix < WeichenZahl ) 
        if ( iniTyp[ adjWix ] != FSERVO ) adjWix = WeichenZahl;
    #endif
}




//////////////////////////////////////////////////////////////////////////
// Allgemeine Unterprogramme 
void setWeichenAddr(void) {
    // Adressmodus aus CV29 auslesen
    isOutputAddr = Dcc.getCV( CV_29_CONFIG ) & CV29_OUTPUT_ADDRESS_MODE;
    // Adresse der 1. Weiche aus Decoderaddresse berechnen
    if ( isOutputAddr ) 
        weichenAddr = Dcc.getAddr( );
    else
        weichenAddr = (Dcc.getAddr( )-1)*4 +1 + rocoOffs ;
}
//--------------------------------------------------------
void softReset(void){
    DB_PRINT( "RESET", 0 );
    delay(1000);
    #ifdef __AVR_MEGA__
    asm volatile ("  jmp 0");
    #endif
    #ifdef __STM32F1__
    nvic_sys_reset();   //System-Reset der STM32-CPU
    #endif
}
//-----------------------------------------------------------
#ifdef DEBUG

void DBprintCV(void) {
    // für Debug-Zwecke den gesamten genutzten CV-Speicher ausgeben
    // Standard-Adressen
   DB_PRINT( "--------- Debug-Ausgabe CV-Werte ---------", 0 );
   DB_PRINT( "Version: %d, ManufactId: %d", Dcc.getCV( CV_VERSION_ID ), Dcc.getCV( CV_MANUFACTURER_ID ) );
   DB_PRINT( "Konfig   (CV29)  : 0x%X", Dcc.getCV( CV_29_CONFIG ) );
   DB_PRINT( "Adresse: (CV1/9) : %d", Dcc.getCV( CV_ACCESSORY_DECODER_ADDRESS_LSB )+Dcc.getCV( CV_ACCESSORY_DECODER_ADDRESS_MSB )*256);
   DB_PRINT( "1.Weichenaddresse: %d", weichenAddr );
    
    // Decoder-Konfiguration global
   DB_PRINT( "Initierungswert: 0x%x (%d) ", Dcc.getCV( (int) &CV->modeVal ), Dcc.getCV( (int) &CV->modeVal ) );
   DB_PRINT( "PoM-Adresse    : %d"   , Dcc.getCV( (int) &CV->PomAddrLow) + 256* Dcc.getCV( (int) &CV->PomAddrHigh ) );
    
    // Output-Konfiguration
   DB_PRINT( "Wadr | Typ | CV's  | Mode | Par1 | Par2 | Par3 | Status |",0 );
    for( byte i=0; i<WeichenZahl; i++ ) {
       DB_PRINT( "%4d |%4d | %2d-%2d | %4d | %4d | %4d | %4d | %3d " , weichenAddr+i, iniTyp[i],
                                                                 &CV->Fkt[i].Mode,  &CV->Fkt[i].State,
                                                                 Dcc.getCV( (int)  &CV->Fkt[i].Mode ),
                                                                 Dcc.getCV( (int)  &CV->Fkt[i].Par1 ),
                                                                 Dcc.getCV( (int)  &CV->Fkt[i].Par2 ),
                                                                 Dcc.getCV( (int)  &CV->Fkt[i].Par3 ),
                                                                 Dcc.getCV( (int)  &CV->Fkt[i].State ) );
    }
    
}
#else
void DBprintCV(void) {
    
}
#endif

