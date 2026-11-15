/* Converted from ComponentTester 1.57m (c) Markus Reschke, EUPL-1.2, by tools/ctport.py.
 * Do not edit: change ct/upstream, tools/ctport.py or the LCR-P1 layer instead. */
/* ************************************************************************
 *
 *   language specific global variables: Russian (Windows-1251)
 *
 *   (c) 2017-2024 by Markus Reschke
 *   translation by indman@EEVblog
 *   based on code from Markus Frejek and Karl-Heinz Kübbeler
 *
 * ************************************************************************ */


/*
 *  Russian
 */

#if defined (UI_RUSSIAN)

  /*
   *  constant strings
   *  - stored in EEPROM/Flash
   */


  /* firmware */
  const unsigned char Tester_str[] MEM_TYPE = "Ìóëüòèòåñòåð";
  

  /* common terms and texts */
  const unsigned char Probing_str[] MEM_TYPE = "Òåñòèðîâàíèå...";
  const unsigned char Timeout_str[] MEM_TYPE = "Âðåìÿ èñòåêëî!";
  const unsigned char Failed1_str[] MEM_TYPE = "Äåòàëè íåò";
  const unsigned char Failed2_str[] MEM_TYPE = "èëè ïîâðåæäåíà";
  const unsigned char Done_str[] MEM_TYPE = "Ãîòîâî!";
  const unsigned char Select_str[] MEM_TYPE = "Ðåæèì";
  const unsigned char Adjustment_str[] MEM_TYPE = "Êîððåêòèðîâêà";
  const unsigned char Save_str[] MEM_TYPE = "Ñîõðàíèòü";
  const unsigned char Load_str[] MEM_TYPE = "Çàãðóçèòü";
  const unsigned char Show_str[] MEM_TYPE = "Äàííûå";
  const unsigned char Remove_str[] MEM_TYPE = "Ðàñêîðîòèòå";
  const unsigned char Create_str[] MEM_TYPE = "Çàêîðîòèòå";
  const unsigned char ShortCircuit_str[] MEM_TYPE = "êîíòàêòû 1-2-3!";
  const unsigned char DischargeFailed_str[] MEM_TYPE = "Áàòàðåÿ?";
  const unsigned char Error_str[] MEM_TYPE = "Îøèáêà!";
  const unsigned char Exit_str[] MEM_TYPE = "Âûõîä";
  const unsigned char Checksum_str[] MEM_TYPE = "Êîíòð.ñóììà";
  const unsigned char BJT_str[] MEM_TYPE = "BJT";
  const unsigned char Thyristor_str[] MEM_TYPE = "Òèðèñòîð";
  const unsigned char Triac_str[] MEM_TYPE = "Ñèìèñòîð";
  const unsigned char PUT_str[] MEM_TYPE = "PUT";
  const unsigned char Bye_str[] MEM_TYPE = "Äî ñâèäàíèÿ!";
  


  /* units */
  const unsigned char Hertz_str[] MEM_TYPE = "Ãö";


  /* options */
  #ifdef SW_SELFTEST
    const unsigned char Selftest_str[] MEM_TYPE = "Ñàìîòåñò";
  #endif

  #if ! defined (BAT_NONE) && ! defined (UI_BATTERY)
    const unsigned char Battery_str[] MEM_TYPE = "Áàò";
    const unsigned char OK_str[] MEM_TYPE = "OK";
    const unsigned char Weak_str[] MEM_TYPE = "ñëàáàÿ";
    const unsigned char Low_str[] MEM_TYPE = "ðàçðÿä";
  #endif

  #ifdef BAT_EXT_UNMONITORED
    const unsigned char External_str[] MEM_TYPE = "âíåøíÿÿ";
  #endif

  #ifdef UI_KEY_HINTS
    const unsigned char Menu_or_Test_str[] MEM_TYPE = "<Ìåíþ Òåñò>";
  #endif

  #if defined (SW_PWM_SIMPLE) || defined (SW_PWM_PLUS)
    const unsigned char PWM_str[] MEM_TYPE = "10-áèò ØÈÌ";
  #endif

  #ifdef SW_SQUAREWAVE
    const unsigned char SquareWave_str[] MEM_TYPE = "Ãåíåðàòîð";
  #endif

  #if defined (HW_ZENER) || defined (HW_PROBE_ZENER)
    const unsigned char Zener_str[] MEM_TYPE = "Âîëüòìåòð";
  #endif

  #if defined (HW_ZENER) && ! defined (ZENER_UNSWITCHED)
    const unsigned char Min_str[] MEM_TYPE = "Ìèí.";
  #endif

  #ifdef HW_FREQ_COUNTER
    const unsigned char FreqCounter_str[] MEM_TYPE = "×àñòîòîìåð";
  #endif

  #ifdef HW_FREQ_COUNTER_EXT
    const unsigned char CounterChannel_str[] MEM_TYPE = "Âõ.êàíàë:";
    const unsigned char FreqInput_str[] MEM_TYPE = "BNC";
    const unsigned char LF_Crystal_str[] MEM_TYPE = "Í× êâàðö";
    const unsigned char HF_Crystal_str[] MEM_TYPE = "Â× êâàðö";
  #endif

  #ifdef HW_RING_TESTER
    const unsigned char RingTester_str[] MEM_TYPE = "Òåñò LOPT/FBT";
  #endif

  #ifdef HW_EVENT_COUNTER
    const unsigned char EventCounter_str[] MEM_TYPE = "Ñ÷åò÷èê";
    const unsigned char Count_str[] MEM_TYPE = "Ïîäñ÷åò";
    const unsigned char Time_str[] MEM_TYPE = "Âðåìÿ";
    const unsigned char Events_str[] MEM_TYPE = "Ñîáûòèÿ";
    const unsigned char Stop_str[] MEM_TYPE = "Ñòîï";
  #endif

  #ifdef HW_LC_METER
    const unsigned char LC_Meter_str[] MEM_TYPE = "LC-ìåòð";
    const unsigned char Adjusting_str[] MEM_TYPE = "íàñòðîéêà...";
  #endif

  #ifdef HW_LOGIC_PROBE
    const unsigned char LogicProbe_str[] MEM_TYPE = "Ëîã.ïðîáíèê";
  #endif

  #ifdef SW_ENCODER
    const unsigned char Encoder_str[] MEM_TYPE = "Ýíêîäåð";
    const unsigned char TurnRight_str[] MEM_TYPE = "Íàïðàâî!";
  #endif

  #ifdef SW_CONTRAST
    const unsigned char Contrast_str[] MEM_TYPE = "Êîíòðàñò";
  #endif

  #if defined (SW_IR_RECEIVER) || defined (HW_IR_RECEIVER)
    const unsigned char IR_Detector_str[] MEM_TYPE = "ÈÊ ïðè¸ìíèê";
  #endif

  #ifdef SW_IR_TRANSMITTER
    const unsigned char IR_Transmitter_str[] MEM_TYPE = "ÈÊ ïåðåäàò÷èê";
    const unsigned char IR_Send_str[] MEM_TYPE = "Êîä.ïîñûëêà...";
  #endif

  #ifdef SW_OPTO_COUPLER
    const unsigned char OptoCoupler_str[] MEM_TYPE = "Oïòðîí";
    const unsigned char None_str[] MEM_TYPE = "íå íàéäåí!";
    const unsigned char CTR_str[] MEM_TYPE = "CTR";
  #endif

  #ifdef VAR_START_STR
    const unsigned char Start_str[] MEM_TYPE = "Ñòàðò";
  #endif

  #ifdef SW_UJT
    const unsigned char UJT_str[] MEM_TYPE = "UJT";
  #endif

  #ifdef SW_SERVO
    const unsigned char Servo_str[] MEM_TYPE = "Ñåðâîïðèâîä";
    const unsigned char Sweep_str[] MEM_TYPE = "<->";
  #endif

  #ifdef SW_CAP_LEAKAGE
    const unsigned char CapLeak_str[] MEM_TYPE = "Òîê óòå÷êè Ñ";
    const unsigned char CapCharge_str[] MEM_TYPE = "Çàðÿä";
    const unsigned char CapHigh_str[] MEM_TYPE = "Rl";
    const unsigned char CapLow_str[] MEM_TYPE = "Rh";
    const unsigned char CapDischarge_str[] MEM_TYPE = "Ðàçðÿä";
  #endif

  #ifdef SW_MONITOR_R
    const unsigned char Monitor_R_str[] MEM_TYPE = "R ìîíèòîð";
  #endif

  #ifdef SW_MONITOR_C
    const unsigned char Monitor_C_str[] MEM_TYPE = "C ìîíèòîð";
  #endif

  #ifdef SW_MONITOR_L
    const unsigned char Monitor_L_str[] MEM_TYPE = "L ìîíèòîð";
  #endif

  #ifdef SW_MONITOR_RCL
    const unsigned char Monitor_RCL_str[] MEM_TYPE = "RCL ìîíèòîð";
  #endif

  #ifdef SW_MONITOR_RL
    const unsigned char Monitor_RL_str[] MEM_TYPE = "RL ìîíèòîð";
  #endif

  #ifdef HW_TOUCH
    const unsigned char TouchSetup_str[] MEM_TYPE = "Íàñòð.ñåíñîðà";
  #endif

  #ifdef SW_POWER_OFF
    const unsigned char PowerOff_str[] MEM_TYPE = "Âûêëþ÷èòü";
  #endif

  #ifdef SW_ONEWIRE_SCAN
    const unsigned char OneWire_Scan_str[] MEM_TYPE = "1-Wire äåòåêò";
    const unsigned char Bus_str[] MEM_TYPE = "Øèíà";
  #endif

  #ifdef SW_CONTINUITY_CHECK
    const unsigned char ContinuityCheck_str[] MEM_TYPE = "Çâóê.ïðîçâîíêà";
  #endif

  #ifdef SW_FONT_TEST
    const unsigned char FontTest_str[] MEM_TYPE = "Øðèôò";
  #endif

  #ifdef SW_SYMBOL_TEST
    const unsigned char SymbolTest_str[] MEM_TYPE = "Ñèìâîëû";
  #endif

  #ifdef HW_FLASHLIGHT
    const unsigned char Flashlight_str[] MEM_TYPE = "Ôîíàðèê";
  #endif

  #ifdef SW_PHOTODIODE
    const unsigned char Photodiode_str[] MEM_TYPE = "Ôîòîäèîä";
    const unsigned char NoBias_str[] MEM_TYPE = "Ip_no";
    const unsigned char ReverseBias_str[] MEM_TYPE = "Ip_rev";
  #endif

  #ifdef SW_DIODE_LED
    const unsigned char Diode_LED_str[] MEM_TYPE = "Äèîä/LED";
  #endif

  #ifdef SW_METER_5VDC
    const unsigned char Meter_5VDC_str[] MEM_TYPE = "5V-âîëüòìåòð";
  #endif

#endif


/* ************************************************************************
 *   EOF
 * ************************************************************************ */
