#ifndef ASCII_TRACE_DEFINITION_H
#define ASCII_TRACE_DEFINITION_H

enum class Trace_Time_Unit { PICOSECOND, NANOSECOND, MICROSECOND, SIMSECOND};//The unit of arrival times in the input file
#define PicoSecondCoeff  1000000000000	//the coefficient to convert picoseconds to second
#define NanoSecondCoeff  1000000000	//the coefficient to convert nanoseconds to second
#define MicroSecondCoeff  1000000	//the coefficient to convert microseconds to second

//  in ../sim/Sim_Def.h
// #define SIM_TIME_TICK_PER_NANOSECOND 4
// #define SimSecondCoeff 1000000000*SIM_TIME_TICK_PER_NANOSECOND

#define ASCIITraceTimeColumn 0
#define ASCIITraceDeviceColumn 1
#define ASCIITraceAddressColumn 2
#define ASCIITraceSizeColumn 3
#define ASCIITraceTypeColumn 4
#define ASCIITraceWriteCode "0"
#define ASCIITraceReadCode "1"
#define ASCIITraceWriteCodeInteger 0
#define ASCIITraceReadCodeInteger 1
#define ASCIILineDelimiter ' '
#define ASCIIItemsPerLine 5

#endif // !ASCII_TRACE_DEFINITION_H
