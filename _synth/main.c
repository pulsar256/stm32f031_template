/*----------------------------------------------------------------------------
/
/
/
/----------------------------------------------------------------------------*/
#include "stm32f0xx.h"
#include "stm32f0xx_rcc.h"
#include "stm32f0xx_spi.h"
#include "stm32f0xx_gpio.h"
#include "stm32f0xx_dma.h"
#include "stm32f0xx_misc.h"
#include "stm32f0xx_usart.h"
/*---------------------------------------------------------------------------*/
#include "lut.h"
#include "synth.h"
#include "math_func.h"
#include "usart1.h"
#include "digital.h"
#include "ringBuffer.h"
#include "xprintf.h"
#include "soft_uart.h"
#include "systick_delay.h"
/*---------------------------------------------------------------------------*/
#define MAX_VOICE 12
#define BURST_SIZE 16
/*---------------------------------------------------------------------------*/
t_lfo outputLfo;
uint8_t midibuf[2];
uint8_t argInd = 0;
RingBuffer_t midi_ringBuf;
uint8_t runningCommand = 0;
uint16_t g_modFreq;
uint16_t g_maxModulation;
t_envSetting fmEnvSetting;
t_envSetting ampEnvSetting;
t_envSetting modEnvSetting;
int16_t g_modulationIndex = 1;
int16_t g_modulationOffset = 0;
struct t_key voice[MAX_VOICE];
uint8_t midi_ringBufData[1024];
int32_t I2S_Buffer_Tx[BURST_SIZE * 2 * 2]; // 2 x 2channels worth of data
/*---------------------------------------------------------------------------*/
static void hardware_init();
static inline void check_notes();
static inline void synth_loop(int32_t* const buf);
static void midi_noteOffMessageHandler(const uint8_t note);
static inline int32_t convertDataForDma_24b(const int32_t data);
static void midi_controlMessageHandler(const uint8_t id, const uint8_t data);
static void midi_noteOnMessageHandler(const uint8_t note,const uint8_t velocity);
static int16_t envelope_iterate(t_envelope* env, const t_envSetting* setting, const uint16_t maxVal);
static int32_t fm_iterate(t_key* key, const int16_t depth_fm, const uint16_t depth_amp, const uint16_t depth_mod);
static inline int32_t find_free_note();

/*---------------------------------------------------------------------------*/
int main(void)
{ 
  int32_t* const Buffer_A = &(I2S_Buffer_Tx[0]);
  int32_t* const Buffer_B = &(I2S_Buffer_Tx[BURST_SIZE * 2]);

  hardware_init();
 
  while(1)
  { 
    uint32_t tmpreg = DMA1->ISR;    

    /* Buffer_A will be read after this point. Start to fill Buffer_B now! */
    if(tmpreg & DMA1_IT_TC3)
    {
      digitalWrite(A,0,HIGH);
      DMA1->IFCR |= DMA1_IT_TC3;      

      synth_loop(Buffer_B);      

      digitalWrite(A,0,LOW);
    }
    /* Buffer_B will be read after this point. Start to fill Buffer_A now! */
    else if(tmpreg & DMA1_IT_HT3)
    {
      digitalWrite(A,0,HIGH);
      DMA1->IFCR |= DMA1_IT_HT3;      

      synth_loop(Buffer_A);      

      digitalWrite(A,0,LOW);
    }    
  }
  
  return 0;
}
/*---------------------------------------------------------------------------*/
static int16_t envelope_iterate(t_envelope* env, const t_envSetting* setting, const uint16_t maxVal)
{  
  int16_t output;

  if(env->state == 0) /* Idle */
  {        
    env->envelopeCounter = 0;   
  }    
  else if(env->state == 1) /* Attack */
  {    
    env->envelopeCounter += setting->attackRate;
    
    /* Check overflow! */
    if(env->envelopeCounter < setting->attackRate)
    {
      env->state = 2;                         
      env->envelopeCounter = 0xFFFF;
    }    
  }
  else if(env->state == 2) /* Decay */
  {    
    /* Detect sustain point */
    if(env->envelopeCounter < (setting->sustainLevel + setting->decayRate))
    {          
      env->state = 3;      
      env->envelopeCounter = setting->sustainLevel;
    }
    else
    {
      env->envelopeCounter -= setting->decayRate;     
    }
  }
  else if(env->state == 3) /* Sustain */
  {    
    /* Wait for note-off event ... */  
  }
  else if(env->state == 4) /* Release */
  {    
    /* Detect underflow ... */
    if(env->envelopeCounter < setting->releaseRate)
    {          
      env->state = 0;      
      env->envelopeCounter = 0;
    }
    else
    {
      env->envelopeCounter -= setting->releaseRate;     
    }
  }
  else /* What? */
  {    
    env->state = 0;      
    env->envelopeCounter = 0;
  }

  output = S16S16MulShift16(env->envelopeCounter, maxVal);
 
  return output;  
}
/*---------------------------------------------------------------------------*/
static inline int32_t fm_iterate(t_key* key, const int16_t depth_fm, const uint16_t depth_amp, const uint16_t depth_mod)
{ 
  int16_t signal_mod;
  int16_t signal_fm;
  uint16_t signal_phase;
    
  key->phaseCounterFm += key->freqMod;
  key->phaseCounterFm += g_modulationOffset;

  key->phaseCounterTone += key->freqTone;  

  signal_fm = sin_lut[key->phaseCounterFm >> 6];
  signal_fm = S16S16MulShift8(signal_fm, depth_fm);

  signal_phase = key->phaseCounterTone;    
  signal_phase += signal_fm;  

  key->phaseCounterMod += g_modFreq;
  signal_mod = sin_lut[key->phaseCounterMod >> 6];
  signal_phase += S16S16MulShift8(signal_mod, depth_mod);
  
  return S16S16MulShift16(sin_lut[signal_phase >> 6], depth_amp);    
}
/*---------------------------------------------------------------------------*/
static inline void check_notes()
{
  uint8_t i;
  uint8_t data;

  /* Analyse MIDI input stream */
  while(1)    
  {       
    __disable_irq();
      if(RingBuffer_GetCount(&midi_ringBuf) == 0) 
      {
        __enable_irq(); 
        break; 
      }    
      data = RingBuffer_Remove(&midi_ringBuf);
    __enable_irq();
      
    if(data & 0x80)
    {
      argInd = 0;
      runningCommand = data;
    }
    else
    {
      midibuf[argInd++] = data;
      if(argInd == 2)
      {               
        argInd = 0;

        /* Note on */
        if((runningCommand & 0xF0) == 0x90)
        {                
          /* If velocity is zero, treat it like 'note off' */
          if(midibuf[1] == 0x00)
          {
            midi_noteOffMessageHandler(midibuf[0]);
          }
          else
          {
            midi_noteOnMessageHandler(midibuf[0],midibuf[1]);
          }
        }
        /* Note off */
        else if((runningCommand & 0xF0) == 0x80)
        {        
          midi_noteOffMessageHandler(midibuf[0]);
        }
        /* Control message */
        else if((runningCommand & 0xF0) == 0xB0)
        {
          midi_controlMessageHandler(midibuf[0],midibuf[1]);            
        }   
      }          
    }    
  }  

  /* Scan voices */
  for(i=0;i<MAX_VOICE;++i)
  {         
    /* Key down */
    if((voice[i].noteState == NOTE_TRIGGER) && (voice[i].noteState_d != NOTE_TRIGGER))
    {
      voice[i].phaseCounterFm = 0;  
      voice[i].phaseCounterMod = 0;      
      voice[i].phaseCounterTone = 0;        

      /* Attack state */
      voice[i].fmEnvelope.state = 1;            
      voice[i].ampEnvelope.state = 1;      
      voice[i].modEnvelope.state = 1;            
      voice[i].fmEnvelope.envelopeCounter= 0;
      voice[i].ampEnvelope.envelopeCounter= 0;
      voice[i].modEnvelope.envelopeCounter = 0;      
    }
    /* Key up */
    else if((voice[i].noteState != NOTE_TRIGGER) && (voice[i].noteState_d == NOTE_TRIGGER))
    {
      /* Release state */
      voice[i].fmEnvelope.state = 4;      
      voice[i].ampEnvelope.state = 4;      
      voice[i].modEnvelope.state = 4;      
    }

    voice[i].noteState_d = voice[i].noteState;
  }
}
/*---------------------------------------------------------------------------*/
static inline void synth_loop(int32_t* const buf)
{
  uint8_t i;
  uint8_t k;
  int32_t* buf_p = buf;    
  int32_t outputBuffer[BURST_SIZE];  

  check_notes();

  /* Deal with the initial voice first */
  {          
    int16_t fm_amp = envelope_iterate(&(voice[0].fmEnvelope),&fmEnvSetting,voice[0].maxModulation);    
    int16_t sig_amp = envelope_iterate(&(voice[0].ampEnvelope),&ampEnvSetting,voice[0].keyVelocity);
    int16_t mod_amp = envelope_iterate(&(voice[0].modEnvelope),&modEnvSetting,g_maxModulation);  

    if(sig_amp == 0)
    {
      voice[0].lastnote = 0;
      voice[0].noteState = NOTE_SILENT;
    } 
    
    for(i=0;i<BURST_SIZE;i++)
    {              
      outputBuffer[i] = fm_iterate(&(voice[0]),fm_amp,sig_amp,mod_amp);
    }
  }
  
  /* Scan through other voices */
  for(k=1;k<MAX_VOICE;k++) 
  {          
    int16_t fm_amp = envelope_iterate(&(voice[k].fmEnvelope),&fmEnvSetting,voice[k].maxModulation);   
    int16_t sig_amp = envelope_iterate(&(voice[k].ampEnvelope),&ampEnvSetting,voice[k].keyVelocity); 
    int16_t mod_amp = envelope_iterate(&(voice[k].modEnvelope),&modEnvSetting,g_maxModulation);  

    if(sig_amp == 0)
    {
      voice[k].lastnote = 0;
      voice[k].noteState = NOTE_SILENT;
    }    

    for(i=0;i<BURST_SIZE;i++)
    {      
      outputBuffer[i] += fm_iterate(&(voice[k]),fm_amp,sig_amp,mod_amp);
    }
  }   

  /* Put the values inside the DMA buffer for later */
  for(i=0;i<BURST_SIZE;i++)
  {    
    int32_t left;
    int32_t input;
    int32_t right;
    int32_t inp_mod;
    int32_t lfoValue;          

    input = outputBuffer[i];

    /* Psuedo stereo panning effect using an LFO with phase counters with slight offsets */
    outputLfo.phaseCounter_left += outputLfo.freq;
    outputLfo.phaseCounter_right = outputLfo.phaseCounter_left + outputLfo.stereoPanning_offset;

    /* Right channel output */
    lfoValue = (sin_lut[outputLfo.phaseCounter_right >> 6] + 32768) >> 8;
    inp_mod = S16S16MulShift8(input,lfoValue);      
    right = S16S16MulShift4(inp_mod,outputLfo.depth) + S16S16MulShift4(input,255-outputLfo.depth);

    /* Left channel output */
    lfoValue = (sin_lut[outputLfo.phaseCounter_left >> 6] + 32768) >> 8;
    inp_mod = S16S16MulShift8(input,lfoValue);
    left = S16S16MulShift4(inp_mod,outputLfo.depth) + S16S16MulShift4(input,255-outputLfo.depth);

    /* Put them on the DMA train */
    *buf_p++ = convertDataForDma_24b(left * 2);
    *buf_p++ = convertDataForDma_24b(right * 2);
  } 
}
/*---------------------------------------------------------------------------*/
static inline int32_t convertDataForDma_24b(const int32_t data)
{      
  uint32_t result;
  uint32_t shiftedData;
  shiftedData = data << 8;
  result = (shiftedData & 0xFFFF0000) >> 16;
  result |= (shiftedData & 0x0000FFFF) << 16;
  return (int32_t)result;  
}
/*---------------------------------------------------------------------------*/
void USART1_IRQHandler()
{
  USART1->ICR = 0xFFFFFFFF;
  uint8_t data = usart1_readByte();
  if((data != 0xF8) && (data != 0xFE))
  {
    if(!RingBuffer_IsFull(&midi_ringBuf))
    {
      RingBuffer_Insert(&midi_ringBuf,data);    
    }  
  }  
}
/*---------------------------------------------------------------------------*/
static void midi_controlMessageHandler(const uint8_t id, const uint8_t data)
{
  
}
/*---------------------------------------------------------------------------*/
static void midi_noteOnMessageHandler(const uint8_t note,const uint8_t velocity)
{
  /* Lokup table has limited range */
  if((note > 20) && (note < 109))
  {
    uint8_t k = find_free_note();

    if(k >=0)
    {
      voice[k].freqTone = noteToFreq[note - 21];
      voice[k].freqMod = S16S16MulShift8(voice[k].freqTone, g_modulationIndex);
      voice[k].lastnote = note;
      voice[k].keyVelocity = log_lut[velocity];
      voice[k].maxModulation = (voice[k].keyVelocity >> 6);
      voice[k].noteState = NOTE_TRIGGER;
      return;
    }

  }  
}
/*---------------------------------------------------------------------------*/
static void midi_noteOffMessageHandler(const uint8_t note)
{
  uint8_t k;

  /* Search for previously triggered key */
  for (k = 0; k < MAX_VOICE; ++k)
  {
    if(voice[k].lastnote == note)
    {
      voice[k].lastnote = 0;
      voice[k].noteState = NOTE_DECAY;             
      break;
    }
  }          
}
/*---------------------------------------------------------------------------*/
/**
 * Returns the array index of the next available, free node or -1 if all
 * voices are busy.
 */
static inline int32_t find_free_note()
{
  uint8_t k;

  // first, try to use true free slot
  for (k = 0; k < MAX_VOICE; ++k)
    if((voice[k].lastnote == 0) && (voice[k].noteState == NOTE_SILENT))
      return k;

  // fallback to first decaying note
  // todo: use the oldest note to kill
  for (k = 0; k < MAX_VOICE; ++k)
    if((voice[k].noteState == NOTE_DECAY))
      return k;

  return -1;
}
/*---------------------------------------------------------------------------*/
static void hardware_init()
{  

  I2S_InitTypeDef I2S_InitStructure;
  DMA_InitTypeDef DMA_InitStructure;

  /* init systick and delay system */
  systick_init(SYSTICK_1MS);  

  /* init uart and enable xprintf library */
  usart1_init(31250);
  xdev_out(usart1_sendChar);
  usart1_enableReceiveISR();

  /* Debug pins ... */
  pinMode(A,9,OUTPUT);
  digitalWrite(A,9,HIGH);

  pinMode(A,0,OUTPUT);
  digitalWrite(A,0,HIGH);

  /* I2S pin: Word select */
  pinMode(A,4,ALTFUNC);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource4, GPIO_AF_0);

  /* I2S pin: Bit clock */
  pinMode(A,5,ALTFUNC);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource5, GPIO_AF_0);
  
  /* I2S pin: Master clock */
  pinMode(A,6,ALTFUNC);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource6, GPIO_AF_0);
  
  /* I2S pin: Data out */
  pinMode(A,7,ALTFUNC);
  GPIO_PinAFConfig(GPIOA, GPIO_PinSource7, GPIO_AF_0);

  /* Enable the DMA1 and SPI1 clocks */
  RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);

  /* I2S peripheral configuration */
  I2S_InitStructure.I2S_CPOL = I2S_CPOL_Low;  
  I2S_InitStructure.I2S_Mode = I2S_Mode_MasterTx;
  I2S_InitStructure.I2S_AudioFreq = I2S_AudioFreq_96k; // Divide this to 2 since we are using 24 bit I2S
  I2S_InitStructure.I2S_DataFormat = I2S_DataFormat_32b;
  I2S_InitStructure.I2S_Standard = I2S_Standard_Phillips;  
  I2S_InitStructure.I2S_MCLKOutput = I2S_MCLKOutput_Enable;
  I2S_Init(SPI1, &I2S_InitStructure);

  /* DMA peripheral configuration */  
  DMA_InitStructure.DMA_BufferSize = BURST_SIZE * 2 * 2 * 2;
  DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;  
  DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
  DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
  DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
  DMA_InitStructure.DMA_Priority = DMA_Priority_VeryHigh;
  DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)I2S_Buffer_Tx;
  DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable; 
  DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&(SPI1->DR);
  DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
  DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
  DMA_Init(DMA1_Channel3, &DMA_InitStructure);

  /* Enable the DMA channel Tx */
  DMA_Cmd(DMA1_Channel3, ENABLE);  

  /* Enable the I2S TX DMA request */
  SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);

  /* Enable the SPI1 Master peripheral */
  I2S_Cmd(SPI1, ENABLE);       

  /* Small delay for general stabilisation */
  _delay_ms(100);

  /* FM modulation freq vs Signal frequency ratio. Will get divided to 256 later */
  g_modulationIndex = 256;
  g_modulationOffset = 3;

  /* Output signal level amplitude envelope */
  ampEnvSetting.attackRate = 1024;  
  ampEnvSetting.decayRate = 8;
  ampEnvSetting.sustainLevel = 0;
  ampEnvSetting.releaseRate = 20;  

  /* Timbre changing frequency modulation level envelope */
  fmEnvSetting.attackRate = 700;
  fmEnvSetting.decayRate = 8;
  fmEnvSetting.sustainLevel = 0;
  fmEnvSetting.releaseRate = 20;

  /* LFO rate frequency modulation envlope */
  g_modFreq = 5;
  g_maxModulation = 200;
  modEnvSetting.attackRate = 5;
  modEnvSetting.decayRate = 32;
  modEnvSetting.sustainLevel = 0;
  modEnvSetting.releaseRate = 1024;
    
  /* Output signal panning and 'tremolo' effect */
  outputLfo.freq = 3;
  outputLfo.depth = 200;
  outputLfo.phaseCounter_left = 0;
  outputLfo.phaseCounter_right = 0;  
  outputLfo.stereoPanning_offset = 0x8000;    

  xfprintf(dbg_sendChar,"> Hello World\r\n");
  RingBuffer_InitBuffer(&midi_ringBuf, midi_ringBufData, sizeof(midi_ringBufData));
}
/*---------------------------------------------------------------------------*/
