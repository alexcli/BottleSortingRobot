/*
 * File:   main.c
 */

#include <xc.h>
#include <stdio.h>
#include "configBits.h"
#include "constants.h"
#include "lcd.h"
#include "I2C.h"
#include "eeprom.h"

#define __delay_1s() for(char i=0;i<100;i++){__delay_ms(10);}
#define __lcd_newline() lcdInst(0b11000000);
#define __lcd_clear() lcdInst(0x01);
#define __lcd_home() lcdInst(0b10000000);
#define __lcd_cursor_back() lcdInst(0b00010000);
#define __lcd_cursor_next() lcdInst(0b00010100);
#define __bcd_to_num(num) (num & 0x0F) + ((num & 0xF0)>>4)*10
#define __btm(num) (num & 0x0F) + ((num & 0xF0)>>4)*10

#define STATE_STOPPED 0
#define STATE_MAIN_MENU 1
#define STATE_RUNNING 2
#define STATE_DONE 3
#define STATE_VIEW_LOG 4
#define STATE_CHOOSE_LOG 5
#define STATE_NO_LOGS 6
#define STATE_CLEAR_LOGS 7
#define STATE_SEND_LOGS 8
#define STATE_SET_TIME 9

void SortDone(void);
void __lcd_new(void);
void StepperMotorRotateUpSlow(void);
void StepperMotorRotateUpFast(void);
void StepperMotorRotateDown1to2(void);
void StepperMotorRotateDown2to3(void);
void BinMotorMoveTo(char n);

const char keys[] = "123A456B789C*0#D"; 
char state = STATE_MAIN_MENU;

char CCW[8] = {0x09,0x01,0x03,0x02,0x06,0x04,0x0c,0x08};
char CW[8] = {0x08,0x0c,0x04,0x06,0x02,0x03,0x01,0x09}; 

unsigned char set_time[13];
unsigned char set_time_cursor;
unsigned char num_entered;

unsigned char time[7];
unsigned char end_time[7];
unsigned char curr_time_elapsed[7];

unsigned char move_to;

unsigned char cap_yop_count;
unsigned char nocap_yop_count;
unsigned char cap_eska_count;
unsigned char nocap_eska_count;
unsigned char bottle_count;

unsigned char bottle_existence_flag;

bool bottle_type_flag; //1 is Yop, 0 is Eska
bool edge_side_sensor_flag;
bool post_side_sensor_flag;

unsigned long no_bottle_time; 

unsigned char run_selected;
unsigned char stat_selected;

unsigned char num_runs_stored;

bool emergency_flag;

void main(void) {
    OSCCON = OSCCON | 0b01110000; 
    // Enable PLL for the internal oscillator, Processor now runs at 32MHZ
    OSCTUNEbits.PLLEN = 1; 
    
    TRISA = 0b11110000; //Set Port A
    TRISB = 0xFF; //Set Port B as all input
    TRISC = 0x00; //Set Port C 
    TRISD = 0b00000011; //Set Port D
    
    I2C_Master_Init(10000); //Initialize I2C Master with 100KHz clock
    LATB = 0x00; 
    LATA = 0x00;      
    LATCbits.LC2 = 0;
    LATCbits.LC1 = 0;

    ADCON0 = 0x00;  //Disable ADC
    ADCON1 = 0b00001111;  //Sets all inputs to be digital instead of analog   
    initLCD();
    INT1IE = 1;
    ei();           //Enable all interrupts
    
    num_runs_stored = Eeprom_ReadByte(0x00);
    if (num_runs_stored == 255){
        num_runs_stored = 0;
        Eeprom_WriteByte(0x00, num_runs_stored);
    }
    
    time[0] = 0;
    end_time[0] = 0;

    char MotorPos = 1;
    no_bottle_time = 0;
    
    LATCbits.LC7 = 1;

    while(1){
        if (state == STATE_MAIN_MENU){
            LATCbits.LC5 = 1; //RC5 Turns on Edge Sensor
            LATCbits.LC6 = 1; //RC6 Turns on Post Sensor
            LATCbits.LC7 = 1; //RC7 Turns on Top Sensor
            
            //Update time:          
            I2C_Master_Start(); 
            I2C_Master_Write(0b11010000); 
            I2C_Master_Write(0x00); 
            I2C_Master_Stop(); 
            
            I2C_Master_Start();
            I2C_Master_Write(0b11010001); 
            for(unsigned char i=0;i<0x06;i++){
                time[i] = I2C_Master_Read(1);
            }
            time[6] = I2C_Master_Read(0);       
            I2C_Master_Stop();
            __lcd_home();
            
            if (time[5] > 9){
                printf("%02x%02x-%02x %02x:%02x:%02x", time[6],time[5],time[4],time[2],time[1],time[0]);
            } else {
                printf("%02x-%x-%02x %02x:%02x:%02x", time[6],time[5],time[4],time[2],time[1],time[0]);
            }
            __lcd_newline();
            printf("A:START B:LOGS  ");
            __delay_1s();
        }

        if (state == STATE_RUNNING){
            I2C_Master_Start(); //Start condition
            I2C_Master_Write(0b11010000); //7 bit RTC address + Write
            I2C_Master_Write(0x00); //Set memory pointer to seconds
            I2C_Master_Stop(); //Stop condition

            //Read Current Time
            I2C_Master_Start();
            I2C_Master_Write(0b11010001); //7 bit RTC address + Read
            for(unsigned char i=0;i<0x06;i++){
                end_time[i] = I2C_Master_Read(1);
            }
            time[6] = I2C_Master_Read(0);       //Final Read without ack
            I2C_Master_Stop();

            int time_elapsed = 0;
            int endmin = __bcd_to_num(end_time[1]);
            int endsec = __bcd_to_num(end_time[0]);
            int min = __bcd_to_num(time[1]);
            int sec = __bcd_to_num(time[0]);
            int time_elapsed = ((endmin - min))*60 + (endsec - sec);
            if ((endmin - min) < 0){
                time_elapsed += 3600;
            }
            
            //End at 3 minutes or once 10 bottles are sorted or we have a motor failure or jam
            if (time_elapsed > 165 || bottle_count == 10 || emergency_flag){
                SortDone();
                continue;
            }
            
            if (MotorPos == 1){

                LATCbits.LC5 = 0; 
                LATCbits.LC6 = 0; 
                LATCbits.LC7 = 1; //RC7 turns on existence sensor

            	if (PORTEbits.RE0 || bottle_existence_flag){ //RE0 is existence sensor
                    no_bottle_time = 0;
                    
                    //Turn off centrifuge when bottle detected:
                    LATCbits.LC2 = 0;
                    LATCbits.LC1 = 0;
                    
                    __lcd_new();
                    printf("Bottle Detected");
                    
                    __delay_ms(500);
                    
                    //Detecting bottle type:
                    while (1){
                        bottle_type_flag = PORTEbits.RE1; //RE1 is type sensor
                        __delay_ms(40);
                        if (PORTEbits.RE1 == bottle_type_flag){
                            break;
                        }
                    }
                    __lcd_new();

                    //Detecting appropriate edge sensor reading:
                    LATCbits.LC5 = 1; //RC5 turns on edge side sensor
                    LATCbits.LC6 = 0; 
                    LATCbits.LC7 = 0;
                        
                    __delay_ms(600);
                    
                    //RA4 and RA5 are connected to edge side sensors
                    while(1){
                        if (bottle_type_flag){
                            edge_side_sensor_flag = PORTAbits.RA4;
                            __delay_ms(40);
                            if (PORTAbits.RA4 == edge_side_sensor_flag){
                                break;
                            }
                        }
                        else {
                            edge_side_sensor_flag = PORTAbits.RA5;
                            __delay_ms(40);
                            if (PORTAbits.RA5 == edge_side_sensor_flag){
                                break;
                            }
                        }
                    }
                    __lcd_new();
                    
                    LATCbits.LC5 = 0; 
                    LATCbits.LC6 = 1; //RC6 turns on post side sensor
                    LATCbits.LC7 = 0;

                    StepperMotorRotateDown1to2();
                    MotorPos = 2;
           		}
                else if (no_bottle_time == 1){
                    LATCbits.LC2 = 1;
                    LATCbits.LC1 = 0;
                    no_bottle_time += 1;
                }

           		else if (no_bottle_time == 5000){ 
           			LATCbits.LC2 = 0;
                    LATCbits.LC1 = 1;  
                    no_bottle_time += 1;
           		}
                
                else if (no_bottle_time == 8500){
                    LATCbits.LC2 = 1;
                    LATCbits.LC1 = 0;
                    no_bottle_time += 1;
                }
                
                else if (no_bottle_time == 13000){ 
           			LATCbits.LC2 = 0;
                    LATCbits.LC1 = 1;  
                    no_bottle_time += 1;
           		}
                
                else if (no_bottle_time == 15000){
           			LATCbits.LC2 = 1;
                    LATCbits.LC1 = 0;  
                    no_bottle_time += 1;
           		}

                
                else if (no_bottle_time >= 21000){ 
                    SortDone();   
           		}


           		else {                    
           			no_bottle_time +=1;
           		}
            }            
            
           

            if (MotorPos == 2) {
                //Move to 1 is Yop and Cap
                //Move to 2 is Yop and No Cap
                //Move to 3 is Eska and Cap
                //Move to 4 is Eska and No Cap
                                

                __delay_ms(500);
                
                //Detecting appropriate post sensor reading:
                while(1){
                    if (bottle_type_flag){
                        post_side_sensor_flag = PORTDbits.RD0;
                        __delay_ms(40);
                        if (PORTDbits.RD0 == post_side_sensor_flag){
                            break;
                        }
                    }
                    else {
                        post_side_sensor_flag = PORTDbits.RD1;
                        __delay_ms(40);
                        if (PORTDbits.RD1 == post_side_sensor_flag){
                            break;
                        }
                    }
                }

                __lcd_new();
                
                bottle_count += 1;
                
                //Logic to determine cap or no cap:
                if (bottle_type_flag){
                    if (edge_side_sensor_flag && post_side_sensor_flag){
                        printf("Yop Cap");
                        move_to = 1;
                        cap_yop_count += 1;
                    }
                    else{
                        printf("Yop No Cap");
                        move_to = 2;
                        nocap_yop_count += 1;
                    }
                }
                else {
                    if (edge_side_sensor_flag || post_side_sensor_flag){
                        printf("Eska Cap");
                        move_to = 3;
                        cap_eska_count += 1;
                    }
                    
                    else {
                        printf("Eska No Cap");
                        move_to = 4;
                        nocap_eska_count += 1;
                    }
                }
                
                StepperMotorRotateDown2to3();
            	MotorPos = 3;
            }

            if (MotorPos == 3){
                //Turn off all sensors:
                LATCbits.LC5 = 0; 
                LATCbits.LC6 = 0;
                LATCbits.LC7 = 0;
                
                
                //Rotate up slowly first to allow for bottle to drop:
            	StepperMotorRotateUpSlow();
                StepperMotorRotateUpSlow();
                for(int i = 0; i < 100; i++){
                    BinMotorMoveTo(move_to);
                }
                
                StepperMotorRotateUpFast(); // Rotate back to initial position
            	MotorPos = 1;
            }
           	  
        }
    
    }
    return;
}

void __lcd_new(void){
    __lcd_home();
    printf("                ");
    __lcd_newline();
    printf("                ");
    __lcd_home();
}

void StepperMotorRotateUpSlow(void){
    LATCbits.LC2 = 0;
    LATCbits.LC1 = 0;
    __lcd_new();
    
    //Rotating Stepper up slowly, holding servo as well to ensure correct bottle drop:
    for(int i = 0; i < 5; i++){
        for(int j = 0; j < 8; j++){
            LATA = CW[j];           
            BinMotorMoveTo(move_to);
        }
        
    }
}

void StepperMotorRotateUpFast(void){
    LATCbits.LC5 = 0; 
    LATCbits.LC6 = 0; 
    LATCbits.LC7 = 1; //RC7 turns on top sensor
    
    //Turn off centrifuge:
    LATCbits.LC2 = 0;
    LATCbits.LC1 = 0;


    __lcd_new();
    
    //Rotating motor upwards, checking for bottle detection as well
    bottle_existence_flag = 0;
    __lcd_new();
    int i = 0;
    while (PORTBbits.RB0 != 0){
        for(int j = 0; j < 8; j++){
            __lcd_new();
            LATA = CW[j];
            __delay_us(2);
            if(PORTEbits.RE0){
                __lcd_new();
                bottle_existence_flag = 1;
            }
        }
        i++;
        
        //Failsafe if jam or motor fail:
        if (i > 300){
            emergency_flag = 1;
            return;
        }
    }
}




void StepperMotorRotateDown1to2(void){
    __lcd_new();
    for(int i = 0; i < 45; i++){ 
        for(int j = 0; j < 8; j++){
            LATA = CCW[j];
            __delay_ms(1);
        }
    }
    __delay_1s();
    __lcd_new();
}

void StepperMotorRotateDown2to3(void){
    LATCbits.LC5 = 0; //RC5 turns on edge side sensor
    LATCbits.LC6 = 0; //RC6 turns on post side sensor
    LATCbits.LC7 = 0;
    for(int i = 0; i < 40; i++){ 
        for(int j = 0; j < 8; j++){
            LATA = CCW[j];
            __delay_ms(2);
        }
    }
    for(int i = 0; i < 40; i++){
        BinMotorMoveTo(move_to);
    } 
}
   
void BinMotorMoveTo(char n){
    switch(n){
        case 1:
            LATC0 = 1;
            __delay_us(800);
            LATC0 = 0;
            __delay_us(19200);
            break;
        case 2:
            LATC0 = 1;
            __delay_us(1450);
            LATC0 = 0;
            __delay_us(18550);
            break;
        case 3:
            LATC0 = 1;
            __delay_us(1770);
            LATC0 = 0;
            __delay_us(18230);
            break;
        case 4:
            LATC0 = 1;
            __delay_us(2400);
            LATC0 = 0;
            __delay_us(17600);
            break;
    }
}



void SortDone(void){
    //Turn off centrifuge:
    LATCbits.LC2 = 0;
    LATCbits.LC1 = 0;
    
    LATA = 0x00; //Release Stepper
    
    //Finding time elapsed:
    I2C_Master_Start(); //Start condition
    I2C_Master_Write(0b11010000); //7 bit RTC address + Write
    I2C_Master_Write(0x00); //Set memory pointer to seconds
    I2C_Master_Stop(); //Stop condition

    I2C_Master_Start();
    I2C_Master_Write(0b11010001); //7 bit RTC address + Read
    for(unsigned char i=0;i<0x06;i++){
        end_time[i] = I2C_Master_Read(1);
    }
    time[6] = I2C_Master_Read(0);  //Final Read without ack
    I2C_Master_Stop();
    
    int time_elapsed = 0;
    int endmin = __bcd_to_num(end_time[1]);
    int endsec = __bcd_to_num(end_time[0]);
    int min = __bcd_to_num(time[1]);
    int sec = __bcd_to_num(time[0]);
    int time_elapsed = ((endmin - min))*60 + (endsec - sec);
    if ((endmin - min) < 0){
        time_elapsed += 3600;
    }
    
    //Storing Data in EEPROM:
    num_runs_stored += 1;
    Eeprom_WriteByte(num_runs_stored*16, time[6]);
    Eeprom_WriteByte(num_runs_stored*16+1, time[5]);
    Eeprom_WriteByte(num_runs_stored*16+2, time[4]);
    Eeprom_WriteByte(num_runs_stored*16+3, time[2]);
    Eeprom_WriteByte(num_runs_stored*16+4, time[1]);
    Eeprom_WriteByte(num_runs_stored*16+5, time[0]);
    Eeprom_WriteByte(num_runs_stored*16+6, time_elapsed);  
    Eeprom_WriteByte(num_runs_stored*16+7, cap_yop_count);
    Eeprom_WriteByte(num_runs_stored*16+8, nocap_yop_count);
    Eeprom_WriteByte(num_runs_stored*16+9, cap_eska_count);
    Eeprom_WriteByte(num_runs_stored*16+10, nocap_eska_count);
    Eeprom_WriteByte(num_runs_stored*16+11, bottle_count);
    Eeprom_WriteByte(num_runs_stored*16+12, 255);
    Eeprom_WriteByte(num_runs_stored*16+13, 255);
    Eeprom_WriteByte(num_runs_stored*16+14, 255);
    Eeprom_WriteByte(num_runs_stored*16+15, 255);
    Eeprom_WriteByte(0x00, num_runs_stored);

    //Displaying done screen:
    __lcd_new();
    printf("DONE (TOOK %ds)", time_elapsed);
    __lcd_newline();
    printf("A:VIEW B:HOME");
    state = STATE_DONE;
}





void interrupt keypressed(void) {
    if(INT1IF){
        unsigned char keypress = (PORTB & 0xF0) >> 4;
        switch (state){
            case STATE_MAIN_MENU:
                if(keys[keypress] == keys[3]){
                    //START RUN:

                    //Ensure there is still room to store runs:
                    if (num_runs_stored == 64){
                        __lcd_new();
                        printf("NO MEMORY");
                        __delay_1s();
                        break;
                    }
                
                    //Read Current Time:
                    I2C_Master_Start(); //Start condition
                    I2C_Master_Write(0b11010000); //7 bit RTC address + Write
                    I2C_Master_Write(0x00); //Set memory pointer to seconds
                    I2C_Master_Stop(); //Stop condition

                    I2C_Master_Start();
                    I2C_Master_Write(0b11010001); //7 bit RTC address + Read
                    for(unsigned char i=0;i<0x06;i++){
                        time[i] = I2C_Master_Read(1);
                    }
                    time[6] = I2C_Master_Read(0);       //Final Read without ack
                    I2C_Master_Stop();
                    
                    //Setting initial counts and flags:
                    cap_eska_count = 0;
					nocap_eska_count = 0;
					cap_yop_count = 0;
					nocap_yop_count = 0;
                    no_bottle_time = 0;
                    bottle_count = 0;
                    emergency_flag = 0;
                    move_to = 2;
                    
                    
                    __lcd_new();
				    printf("RUNNING...");
                    __lcd_newline();
                    
                    StepperMotorRotateUpFast(); //Ensuring stepper in correct starting position
                    
				    state = STATE_RUNNING;
                }

                else if (keys[keypress] == keys[7]){
                    //ENTER LOG SELECT MENU
                    
                    if (num_runs_stored == 0){
                        state = STATE_NO_LOGS;
                        __lcd_new();
                        printf("NO RUNS YET");
                        __lcd_newline();
                        printf("A:HOME");
                    } else {
                        state = STATE_CHOOSE_LOG;
                        run_selected = num_runs_stored;
                        __lcd_new();
                        time[6] = Eeprom_ReadByte(run_selected*16);
                        time[5] = Eeprom_ReadByte(run_selected*16+1);
                        time[4] = Eeprom_ReadByte(run_selected*16+2);
                        time[2] = Eeprom_ReadByte(run_selected*16+3);
                        time[1] = Eeprom_ReadByte(run_selected*16+4);
                        time[0] = Eeprom_ReadByte(run_selected*16+5);
                        if (time[5] > 9){
                            printf("%02x%02x-%02x %02x:%02x:%02x", time[6],time[5],time[4],time[2],time[1],time[0]);
                        } else {
                            printf("%02x-%x-%02x %02x:%02x:%02x", time[6],time[5],time[4],time[2],time[1],time[0]);
                        }
                        __lcd_newline();
                        printf("A:VU B:NXT C:HME");
                    }
                }
                
                else if (keys[keypress] == keys[12]){
                    //ENTER PC INTERFACE
                    
                    __lcd_new();
                    printf("CONNECT TO PC");
                    __lcd_newline();
                    printf("A:SEND B:BACK");     
                    state = STATE_SEND_LOGS;
                }

                else if (keys[keypress] == keys[13]){
                    //ENTER CLEAR LOGS MENU
                    
                    __lcd_new();
                    printf("CLEAR ALL LOGS?");
                    __lcd_newline();
                    printf("A:CLEAR B:BACK");     
                    state = STATE_CLEAR_LOGS;
                }
                
                else if (keys[keypress] == keys[14]){
                    //ENTER SET DATE AND TIME MENU
                    
                    lcdInst(0b00001111);
                    __lcd_new();
                    set_time_cursor = 0;
                    printf("SETTIME C:CANCEL");
                    __lcd_newline();
                    printf("  -  -     :  ");  
                    __lcd_home();
                    __lcd_newline();
                    state = STATE_SET_TIME;
                }
                break; 

            case STATE_DONE:
                if((keys[keypress]) == keys[3]){
                	run_selected = num_runs_stored;
                    state = STATE_VIEW_LOG;
                    stat_selected = 1;
                    __lcd_new();
                    printf("TOTAL BOTTLES:%d", Eeprom_ReadByte(run_selected*16+11));
	                __lcd_newline();
	                printf("A:NXTSTAT B:HOME");
                }

                if((keys[keypress]) == keys[7]){
                	state = STATE_MAIN_MENU;
                }
                break;

            case STATE_CHOOSE_LOG:
                if((keys[keypress]) == keys[3]){
                    //ENTER VIEWING SPECIFIC LOG MENU
                    
                    state = STATE_VIEW_LOG;
                    stat_selected = 1;
                    __lcd_new();
                    printf("TOTAL BOTTLES:%d", Eeprom_ReadByte(run_selected*16+11));
	                __lcd_newline();
	                printf("A:NXTSTAT B:HOME");
                }

                if((keys[keypress]) == keys[7]){
                    //CYCLE BETWEEN STORED LOGS
                    
                    __lcd_home();
                	if (run_selected == 1){
                		run_selected = num_runs_stored;
                	}
                	else {
                		run_selected -= 1;
                	}
                    time[6] = Eeprom_ReadByte(run_selected*16);
                    time[5] = Eeprom_ReadByte(run_selected*16+1);
                    time[4] = Eeprom_ReadByte(run_selected*16+2);
                    time[2] = Eeprom_ReadByte(run_selected*16+3);
                    time[1] = Eeprom_ReadByte(run_selected*16+4);
                    time[0] = Eeprom_ReadByte(run_selected*16+5);
                    if (time[5] > 9){
                        printf("%02x%02x-%02x %02x:%02x:%02x", time[6],time[5],time[4],time[2],time[1],time[0]);
                    } else {
                        printf("%02x-%x-%02x %02x:%02x:%02x", time[6],time[5],time[4],time[2],time[1],time[0]);
                    }

                }

                if ((keys[keypress]) == keys[11]){
                	state = STATE_MAIN_MENU;
                }
                break;

            case STATE_VIEW_LOG:
            	if((keys[keypress]) == keys[3]){
                    //CYCLE BETWEEN STATISTICS ABOUT SELECTED LOG
                    
            		__lcd_home();

                    if (stat_selected == 7){
                    	stat_selected = 1;
                    }
                    else {
                    	stat_selected += 1;
                    }

                    switch (stat_selected){
                        case 1:
                            printf("TOTAL BOTTLES:%d     ", Eeprom_ReadByte(run_selected*16+11));
	                		break;
	                	case 2:
	                		printf("YOP CAP:%d          ", Eeprom_ReadByte(run_selected*16+7));
	                		break;
	                	case 3:
	                		printf("YOP NO CAP:%d      ", Eeprom_ReadByte(run_selected*16+8));
	                		break;
	                	case 4:
	                		printf("ESKA CAP:%d      ", Eeprom_ReadByte(run_selected*16+9));
	                		break;
	                	case 5:
	                		printf("ESKA NO CAP:%d     ", Eeprom_ReadByte(run_selected*16+10));
	                		break;
	                	case 6:
	                		printf("RUN TIME:%ds      ", Eeprom_ReadByte(run_selected*16+6));
	                		break;
	                	case 7:
                            if (time[5] > 9){
                                printf("%02x%02x-%02x %02x:%02x:%02x", time[6],time[5],time[4],time[2],time[1],time[0]);
                            } else {
                                printf("%02x-%x-%02x %02x:%02x:%02x", time[6],time[5],time[4],time[2],time[1],time[0]);
                            }
	                		break;
                	}

                }

                if((keys[keypress]) == keys[7]){
                	state = STATE_MAIN_MENU;
                }
                break;
                
            case STATE_NO_LOGS:
                if((keys[keypress]) == keys[3]){
            		state = STATE_MAIN_MENU;
                }
                break;
            
            case STATE_CLEAR_LOGS:
                if((keys[keypress]) == keys[3]){
                    num_runs_stored = 0;
                    Eeprom_WriteByte(0x00, num_runs_stored);
                    __lcd_new();
                    printf("ALL LOGS CLEARED");
                    __delay_1s();
                    state = STATE_MAIN_MENU;
                }
                else{
                    state = STATE_MAIN_MENU;
                }
                break;
                
            case STATE_SEND_LOGS:       
                if((keys[keypress]) == keys[3]){
                    
                    __lcd_new();
                    printf("PREPARING...");
                    
                    __delay_1s();
                    
                    __lcd_new();
                    printf("TRANSFERRING...");
                    int temp;
                    for (int i = 16; i < (num_runs_stored + 1)*16; i++){
                        if ((i % 16) < 6){
                            //First six statistics are stored as BCD so convert:
                            temp = __bcd_to_num(Eeprom_ReadByte(i));
                        } else {
                            temp = Eeprom_ReadByte(i);
                        }
                        I2C_Master_Start(); //Start condition
                        I2C_Master_Write(0b00010000); //7 bit RTC address + Write
                        I2C_Master_Write(temp); //7 bit RTC address + Write
                        I2C_Master_Stop();
                        __delay_ms(15);
                    }
                    I2C_Master_Start(); //Start condition
                    I2C_Master_Write(0b00010000); //7 bit RTC address + Write
                    I2C_Master_Write(250); //7 bit RTC address + Write
                    I2C_Master_Stop();
                    __delay_ms(15);

                    __lcd_new();
                    printf("DONE");
                    __delay_1s();
                    
                    state = STATE_MAIN_MENU;
                }
                else{
                    state = STATE_MAIN_MENU;
                }
                break;
                
            case STATE_SET_TIME:                
                if((keys[keypress]) == keys[0]){
                    set_time_cursor += 1;
                    num_entered = 1;
                }
                else if((keys[keypress]) == keys[1]){
                    set_time_cursor += 1;
                    num_entered = 2;
                }
                else if((keys[keypress]) == keys[2]){
                    set_time_cursor += 1;
                    num_entered = 3;
                }
                else if((keys[keypress]) == keys[4]){
                    set_time_cursor += 1;
                    num_entered = 4;
                }                
                else if((keys[keypress]) == keys[5]){
                    set_time_cursor += 1;
                    num_entered = 5;
                }                
                else if((keys[keypress]) == keys[6]){
                    set_time_cursor += 1;
                    num_entered = 6;
                }
                else if ((keys[keypress]) == keys[7]){
                    if ((set_time_cursor == 2)||(set_time_cursor == 4)||(set_time_cursor == 6)||(set_time_cursor == 8)||(set_time_cursor == 10)){
                        __lcd_cursor_back();
                    }
                    if (set_time_cursor != 0){
                        set_time_cursor -=1;
                        __lcd_cursor_back();
                    }
                }
                else if((keys[keypress]) == keys[8]){
                    set_time_cursor += 1;
                    num_entered = 7;
                }
                else if((keys[keypress]) == keys[9]){
                    set_time_cursor += 1;
                    num_entered = 8;
                }
                else if((keys[keypress]) == keys[10]){
                    set_time_cursor += 1;
                    num_entered = 9;
                }  
                
                else if((keys[keypress]) == keys[11]){
                    lcdInst(0b00001100);
                    state = STATE_MAIN_MENU;
                }  

                else if((keys[keypress]) == keys[13]){
                    set_time_cursor += 1;
                    num_entered = 0;
                }
                
                if (((keys[keypress]) != keys[7]) && ((keys[keypress]) != keys[3]) && ((keys[keypress]) != keys[12]) && ((keys[keypress]) != keys[14]) && ((keys[keypress]) != keys[15])){
                    set_time[set_time_cursor] = num_entered;
                    printf("%x", num_entered);
                    if ((set_time_cursor == 2)||(set_time_cursor == 4)||(set_time_cursor == 6)||(set_time_cursor == 8)||(set_time_cursor == 10)){
                        __lcd_cursor_next();
                    }
                }
                
                if (set_time_cursor == 10){
                    
                    //Taking care of invalid date and time entries:
                    if ((set_time[9] > 5)||(set_time[7] > 2)||(set_time[5] > 3)||(set_time[3] > 1)||
                        ((set_time[7] == 2) && (set_time[8] > 3))||((set_time[5] == 3) && (set_time[6] > 1))||
                        ((set_time[3] == 1) && (set_time[4] > 2))||((set_time[3] == 0) && (set_time[4] == 0))||
                        ((set_time[5] == 0) && (set_time[6] == 0))){
                        lcdInst(0b00001100);
                        __lcd_new();
                        printf("NOT VALID");
                        __delay_1s();
                        state = STATE_MAIN_MENU;
                    }
                    
                    else if (((set_time[4] == 4)||(set_time[4] == 6)||(set_time[4] == 9)||((set_time[3] == 1) && set_time[4] == 1)) && ((set_time[5]*10 + set_time[6]) > 30)){
                        lcdInst(0b00001100);
                        __lcd_new();
                        printf("NOT VALID");
                        __delay_1s();
                        state = STATE_MAIN_MENU;
                    }
                    
                    else if ((set_time[3] == 0) && (set_time[4] == 2) && ((set_time[5] * 10 + set_time[6]) > 29)){
                        lcdInst(0b00001100);
                        __lcd_new();
                        printf("NOT VALID");
                        __delay_1s();
                        state = STATE_MAIN_MENU;
                    } 
                    
                    else if ((set_time[3] == 0) && (set_time[4] == 2) && ((set_time[5] * 10 + set_time[6]) > 28) && ((set_time[1]*10+set_time[2])% 4 != 0)){
                         lcdInst(0b00001100);
                        __lcd_new();
                        printf("NOT VALID");
                        __delay_1s();
                        state = STATE_MAIN_MENU;
                    }
                    else {
                        time[0] = 0x00; //45 Seconds 
                        time[1] = set_time[9]*16 + set_time[10]; //59 Minutes
                        time[2] = set_time[7]*16 + set_time[8]; //24 hour STATE, set to 23:00
                        time[3] = 0x07;
                        time[4] = set_time[5]*16 + set_time[6]; //31st
                        time[5] = set_time[3]*16 + set_time[4]; //December
                        time[6] = set_time[1]*16 + set_time[2];//2016
                        I2C_Master_Start(); //Start condition
                        I2C_Master_Write(0b11010000); //7 bit RTC address + Write
                        I2C_Master_Write(0x00); //Set memory pointer to seconds
                        for(char i=0; i<7; i++){
                            I2C_Master_Write(time[i]);
                        }    
                        I2C_Master_Stop(); //Stop condition
                        lcdInst(0b00001100);
                        state = STATE_MAIN_MENU;
                    }
                }
                break;
        }

        INT1IF = 0;     //Clear flag bit
    }
}


