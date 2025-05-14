/* Environment includes. */
#include "DriverLib.h"

/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timer.h"

/* DEFINES */
/* UART configuration - note this does not use the FIFO so is not very
 * efficient. */
#define mainBAUD_RATE           ( 19200 )
/* Task priorities. */
#define mainCHECK_TASK_PRIORITY ( tskIDLE_PRIORITY + 3 )
/* Temps */
#define MAX_TEMP_DECIMALS       ( 150 )
#define MIN_TEMP_DECIMALS       ( 0 )
#define INITIAL_TEMP_DECIMALS   ( (MAX_TEMP_DECIMALS + MIN_TEMP_DECIMALS) / 2 )
#define TEMP_DECIMALS_STEP      ( 30 )
#define TEMP_QUEUE_SIZE         ( 3 )
#define MAX_NUMBER_OF_SAMPLES   ( 20 )
#define MIN_NUMBER_OF_SAMPLES   ( 1 )
#define SENSOR_FRECUENCY_HZ     ( 10 )
#define SENSOR_DELAY_MS         ( 1000 / SENSOR_FRECUENCY_HZ )
/* Display */
#define LCD_COLUMNS_FOR_GRAPH   ( 69 )
/* Timer */
/* SysClck = 6.000.000Hz =>
 * 6.000.000 Hz * 10us = 6.000.000 / 10.000 = 60 ticks */
#define TIMER_TICK_COUNT        ( configCPU_CLOCK_HZ / 10000 )
/* Top */
#define TOP_TASK_DELAY_MS       ( 3000 )

/* GLOBALS */
/* Temp */
static QueueHandle_t s_temps_queue;
static QueueHandle_t s_temps_to_display_queue;
static volatile uint8_t s_numberOfSamples = 10; // Number of samples to average
/* UART */
static unsigned char s_uart_buffer[3] = {};
static uint8_t s_uart_index = 0;
/* Timer */
static volatile uint32_t s_overflow_counter = 0;
/* Top Task */
static tBoolean s_is_top_running = false;
static TaskHandle_t xTopTask = NULL;

/* TASKS */
static void vSensorTask(void *pvParameters);
static void vAverageTask(void *pvParameters);
static void vDisplayTask(void *pvParameters);
static void vTopTask(void * pvParameters);

/* HANDLERS*/
void vTimer0A_Handler(void);

/* SET UPS */
static void prvSetupHardware(void);
void vSetupRunTimeStatsTimer(void);

/* FUNCTIONS */
/* Strings */
static void printString (const char * str);
static void printFormat (const char * format, void **args);
static char* intToString(long int num);
static uint8_t lenStr (unsigned char *str);
/* Arrays */
static void appendToArray(uint8_t array[], uint8_t new_value, uint8_t size);
static uint8_t avgArray(uint8_t array[], uint8_t size, uint8_t to_use);
/* Timer */
unsigned long ulGetRunTimeCounterValue ( void );

int main(void)
{
    /* Configure UART and LCD. */
    prvSetupHardware();

    /* Initialize temps queues*/
    s_temps_queue = xQueueCreate(TEMP_QUEUE_SIZE, sizeof(uint8_t));
    s_temps_to_display_queue = xQueueCreate(TEMP_QUEUE_SIZE, sizeof(uint8_t));
    
    // Create tasks
    xTaskCreate(vSensorTask, "SensorGen", configMINIMAL_STACK_SIZE / 2,
        NULL, mainCHECK_TASK_PRIORITY + 1, NULL);
    xTaskCreate(vAverageTask, "AverageCalc", configMINIMAL_STACK_SIZE / 2,
        NULL, mainCHECK_TASK_PRIORITY, NULL);
    xTaskCreate(vDisplayTask, "DisplayGraph", configMINIMAL_STACK_SIZE / 2,
        NULL, mainCHECK_TASK_PRIORITY - 1, NULL);
    xTaskCreate(vTopTask, "TopTask", configMINIMAL_STACK_SIZE,
        NULL, mainCHECK_TASK_PRIORITY - 2, &xTopTask);
    vTaskSuspend(xTopTask);

    /* Start the scheduler. */
    vTaskStartScheduler();

    /* Will only get here if there was insufficient heap to start the
     * scheduler. */
    return 0;
}

/* SET UPS */
static void prvSetupHardware(void)
{
    /* Setup UART */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART0);
    UARTConfigSet(UART0_BASE, mainBAUD_RATE, 
        (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE));
    UARTIntEnable(UART0_BASE, UART_INT_RX);
    IntPrioritySet(INT_UART0, configKERNEL_INTERRUPT_PRIORITY);
    IntEnable(INT_UART0);

    /* Initialise the LCD> */
    OSRAMInit(true);
}

void vSetupRunTimeStatsTimer(void) {
    /* Setup Timer0A to count 10us */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
    TimerConfigure(TIMER0_BASE, TIMER_CFG_32_BIT_PER);
    TimerLoadSet(TIMER0_BASE, TIMER_A, TIMER_TICK_COUNT);

    TimerIntRegister(TIMER0_BASE, TIMER_A, vTimer0A_Handler);
    TimerIntEnable(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    IntMasterEnable();
    IntEnable(INT_TIMER0A);
    TimerEnable(TIMER0_BASE, TIMER_A);
}

/* TASKS */
/** vSensorTask
 * Create new temperature measurements, between values defined in 
 * MAX_TEMP_DECIMALS and MIN_TEMP_DECIMALS, with a defined frequency in
 * SENSOR_FRECUENCY_HZ. The new values ​​are placed into the global queue
 * defined as s_temps_queue.
 */
static void vSensorTask(void *pvParameters)
{
    /* Get tick count to control delay beteween new values */
    TickType_t last_call = xTaskGetTickCount();
    /* Initialize initial tempareture and ramdomize tool */
    uint8_t temp_decimals = INITIAL_TEMP_DECIMALS;
    unsigned long xorshift_state = SysTickValueGet();
    if (xorshift_state == 0)
        xorshift_state = 1;

    while (true)
    {
        /* Wait for next iteration */
        xTaskDelayUntil(&last_call, pdMS_TO_TICKS(SENSOR_DELAY_MS));

        /* Initialize edges of temps */
        int16_t lower = temp_decimals - TEMP_DECIMALS_STEP;
        int16_t upper = temp_decimals + TEMP_DECIMALS_STEP;

        /* Verify limits */
        if (lower < MIN_TEMP_DECIMALS)
            lower = MIN_TEMP_DECIMALS;
        if (upper > MAX_TEMP_DECIMALS)
            upper = MAX_TEMP_DECIMALS;

        /* Calculate a new random value from last tempareture base */
        xorshift_state ^= xorshift_state << 13;
        xorshift_state ^= xorshift_state >> 17;
        xorshift_state ^= xorshift_state << 5;

        temp_decimals = (xorshift_state % (upper - lower + 1)) + lower;

        /* Put new temp into s_temps_queue */
        xQueueSend(s_temps_queue, &temp_decimals, portMAX_DELAY);
    }
}

/** vAverageTask
 * Get values from global queue 's_temps_queue', use the last N values for
 * calculate an average and save this value into the global queue
 * 's_temps_to_display_queue'.
 */
static void vAverageTask(void *pvParameters)
{
    /* Uses to get temp from 's_temps_queue' and to save the average */
    uint8_t new_temp, average; 
    /* Stores the last MAX_NUMBER_OF_SAMPLES historical temperature values */
    uint8_t temps_array[MAX_NUMBER_OF_SAMPLES] = {};

    while (true)
    {
        /* Wait until a new value is in the queue */
        xQueueReceive(s_temps_queue, &new_temp, portMAX_DELAY);

        /* Add new value to the array and calculate the average */
        appendToArray(temps_array, new_temp, MAX_NUMBER_OF_SAMPLES);
        average = avgArray(temps_array, MAX_NUMBER_OF_SAMPLES, s_numberOfSamples);

        /* Send the average to the display queue */
        xQueueSend(s_temps_to_display_queue, &average, portMAX_DELAY);
    }
}

/** vDisplayTask
 * Get values from global queue 's_temps_to_display_queue' to display its in
 * the LCD panel. Also displays the actual value of N and a y axis its take
 * values into the follow interval [0, 1, 2,...,16].
 */
static void vDisplayTask(void *pvParameters)
{
    /* Vars to get the new value to diaplay and an historical array of temps */
    uint8_t new_temp;
    uint8_t temps_array[LCD_COLUMNS_FOR_GRAPH] = {};
    /* Numerical value of column. */
    uint16_t col;
    /* String to display the N current value. */
    char display_N_buffer[5] = {'N', '=', '\0', '\0', '\0'};
    
    while (true)
    {
        /* Wait until a new value is in the queue and add it to the array. */
        xQueueReceive(s_temps_to_display_queue, &new_temp, portMAX_DELAY);
        appendToArray(temps_array, new_temp / 10, LCD_COLUMNS_FOR_GRAPH);

        /* Clear the LCD panel */
        OSRAMClear();

        /* Put N value into the string to display it. */
        display_N_buffer[2] = (char)(s_numberOfSamples / 10 + '0');
        display_N_buffer[3] = (char)(s_numberOfSamples % 10 + '0');

        /* Display N value, display 'y' axis and display a circle representing
         * the zero value of the axis. */
        OSRAMStringDraw(display_N_buffer, 1, 0);
        OSRAMImageDraw("\x70\x88\x88\x70", 20, 1, 4, 1);
        OSRAMImageDraw("\xFF\xFF", 25, 0, 1, 2);

        /* Create and display a column (with the temp value) for all the
         * historical temps values. */
        for (int i = 0; i < LCD_COLUMNS_FOR_GRAPH; i++) 
        {
            col = (1 << temps_array[i]);
            if (i != 0)
            {
                int8_t diff = temps_array[i] - temps_array[i - 1];
                if (diff != 0)
                {
                    col = 0;
                    if (diff < 0)
                        diff *= -1;
                    for (int8_t j = 0; j < diff; j++)
                    {
                        col = col << 1;
                        col++;
                    }
                    col = col << (temps_array[i] < temps_array[i - 1] ?
                        temps_array[i] : temps_array[i - 1]);
                }
            }

            /* Since the column is built in reverse of how it is shown, it is
             * necessary to invert it. */
            col = ((col >> 1) & 0x5555) | ((col << 1) & 0xAAAA);
            col = ((col >> 2) & 0x3333) | ((col << 2) & 0xCCCC);
            col = ((col >> 4) & 0x0F0F) | ((col << 4) & 0xF0F0);
            col = ((col >> 8) & 0x00FF) | ((col << 8) & 0xFF00);

            /* Display the column. */
            OSRAMImageDraw((char *) &col, i + 26, 0, 1, 2);
        }
    }
}

/** vTopTask
 * The task print periodically information about the task existing in the
 * system and the state of the system heap.
 */
static void vTopTask(void * pvParameters)
{
    /* Vars to get the tasks status and its number. */
    TaskStatus_t *px_task_status_array;
    volatile UBaseType_t ux_array_size;    
    unsigned long ul_stats_as_percentage;

    /* Var used to record values of the last iteration. */
    unsigned long *ul_run_time_conters_last;
    uint32_t last_mark_time_counter;
    TickType_t last_call = xTaskGetTickCount();

    /* Print args for printFormat functions, the max number needed in the task
     * its 4. */
    void** print_args = (void**)pvPortMalloc(4 * sizeof(void *));

    /* Get number of tasks */
    ux_array_size = uxTaskGetNumberOfTasks();

    /* Allocate a TaskStatus_t structure for each task. An array could be
     * allocated statically at compile time. */
    px_task_status_array = pvPortMalloc(ux_array_size * sizeof(TaskStatus_t));
    ul_run_time_conters_last = pvPortMalloc(ux_array_size * sizeof(unsigned long));

    for (uint8_t x = 0; x < ux_array_size; x++) ul_run_time_conters_last[x] = 0;

    while (true) {
        if (px_task_status_array == NULL) {
            vPortFree(px_task_status_array);
            vPortFree(ul_run_time_conters_last);
            vPortFree(print_args);
            break;
        }

        /* Print table header. */
        printString("+--------------+--------+---------------------------------+\r\n");
        printString("|     TASK     |  CPU   |       STACK (BYTES) (PERC)      |\r\n");
        printString("|     NAME     |  USE%  | TOTAL | NOW | PERC | MAX | PERC |\r\n");
        printString("+--------------+--------+-------+-----+------+-----+------+\r\n");

        /* Generate raw status information about each task. */
        ux_array_size = uxTaskGetSystemState(px_task_status_array, ux_array_size, NULL);
        for (uint8_t x = 0; x < ux_array_size; x++)
        {                
            /* Print task name. */
            print_args[0] = px_task_status_array[x].pcTaskName;
            printFormat("| %13s|", print_args);

            /* Print CPU use in percentages. */
            ul_stats_as_percentage = (px_task_status_array[ x ].ulRunTimeCounter - ul_run_time_conters_last[x]) * 1000;
            ul_run_time_conters_last[x] = px_task_status_array[ x ].ulRunTimeCounter;
            
            /* Avoid overflow of timer counter. */
            if (s_overflow_counter >= last_mark_time_counter) 
                ul_stats_as_percentage /= (s_overflow_counter - last_mark_time_counter);
            else
                ul_stats_as_percentage /= (0xFFFFFFFF - last_mark_time_counter) + s_overflow_counter;
            
            /* If percentage is greater than 10 (1%) print the valie with
             * one decimal. */
            if (ul_stats_as_percentage >= 10) {
                uint8_t integer_part = ul_stats_as_percentage / 10;
                print_args[0] = (uint8_t *) &integer_part;
                ul_stats_as_percentage = ul_stats_as_percentage % 10;
                print_args[1] = (uint8_t *) &ul_stats_as_percentage;

                printFormat("%-4c,%c%% |", print_args);
            } else {
                printString("   < 1% |");
            }

            /* Print memory stack use for each task. */
            uint8_t total_mem = px_task_status_array[x].pxEndOfStack - px_task_status_array[x].pxStackBase;
            uint8_t now_mem = px_task_status_array[x].pxEndOfStack - px_task_status_array[x].pxTopOfStack;
            uint8_t max_mem = total_mem - px_task_status_array[x].usStackHighWaterMark;
            uint16_t print_mem = total_mem * 2;

            /* Print the total memory stack assigned to this task in bytes. */
            print_args[0] = (uint16_t *) &print_mem;
            printFormat("%-6h |", print_args);
            
            /* Print the current memory stack used for this task in bytes. */
            print_mem = now_mem * 2;
            print_args[0] = (uint16_t *) &print_mem;
            printFormat("%-4h |", print_args);

            /* Print the current memory stack used for this task in percentage. */
            print_mem = (now_mem * 100) / total_mem;
            print_args[0] = (uint16_t *) &print_mem;
            printFormat("%-4h%% |", print_args);
            
            /* Print the maximun historical memory stack used for this task in bytes. */
            print_mem = max_mem * 2;
            print_args[0] = (uint16_t *) &print_mem;
            printFormat("%-4h |", print_args);
            
            /* Print the maximun historical memory stack used for this task in percentage. */
            print_mem = (max_mem * 100) / total_mem;
            print_args[0] = (uint16_t *) &print_mem;
            printFormat("%-4h%% |\r\n", print_args);
        } 
        /* Print header for heap information. */
        printString("+--------------+--------+-------+-----+------+-----+------+\r\n");
        printString("+------------------------------------+------+------+------+\r\n");
        printString("|             HEAP USAGE             |  TOT |  USE | FREE |\r\n");
        printString("+------------------------------------+------+------+------+\r\n");

        /* Calculate the usage heap percentage using free heap and total heap. */
        size_t free_heap = xPortGetFreeHeapSize();
        size_t total_heap = configTOTAL_HEAP_SIZE;
        size_t use_heap = total_heap - free_heap;
        uint8_t heap_use_percentage = (use_heap * 100) / total_heap;

        /* Print heaps values in a charge bar. */
        print_args[0] = (uint8_t *) &heap_use_percentage;
        print_args[1] = (uint32_t *) &total_heap;
        print_args[2] = (uint32_t *) &use_heap;
        print_args[3] = (uint32_t *) &free_heap;

        printString("|    [");
        for (uint8_t i = 0; i < heap_use_percentage; i += 5) UARTCharPut(UART0_BASE, '|');
        for (uint8_t i = heap_use_percentage; i < 100; i += 5) UARTCharPut(UART0_BASE, ' ');

        /* Print heaps values in bytes. */
        printFormat("%-3c %%]    | %-4d | %-4d | %-4d |\r\n", print_args);
        printString("+------------------------------------+------+------+------+\r\n");

        last_mark_time_counter = s_overflow_counter;
        vTaskDelay(pdMS_TO_TICKS(TOP_TASK_DELAY_MS));  
    }
}

/* FUNCTIONS */
/** printString 
 * \brief Print via UART the string passed as parameter. 
 * \param str String to be printed.
 */
static void printString (const char * str) {
    while (*str != '\0') UARTCharPut(UART0_BASE, *(str++));
}

/** intToString 
 * \brief Convert a integer to a string.
 * \param num Number to be convert.
 * \return A pointer to the string that should be freed.
 */
static char* intToString(long int num) {
    long int temp = num;
    int digits = 0;
    if (num == 0) {
        digits = 1;
    } else {
        while (temp != 0) {
            temp /= 10;
            digits++;
        }
    }
    char* str = (char*)pvPortMalloc((digits + 1) * sizeof(char));
    if (str == NULL) {
        return NULL;
    }
    str[digits] = '\0';
    if (num == 0) {
        str[0] = '0';
    } else {
        for (int i = digits - 1; i >= 0; i--) {
            str[i] = (num % 10) + '0';
            num /= 10;
        }
    }

    return str;
}

/** appendToArray 
 * \brief Append a uint8_t into an array, tranforming the array into a FIFO
 * queue.
 * \param array Array where number will be saved.
 * \param new_value Number to append in the array.
 * \param size Size of the array.
 */
static void appendToArray(uint8_t array[], uint8_t new_value, uint8_t size)
{
    for (uint8_t i = 0; i < size - 1; i++)
        array[i] = array[i + 1];
    array[size - 1] = new_value;
}

/** avgArray 
 * \brief Calculates the average value using the x most recent values in the
 * queue-array.
 * \param array Array to get values for the average.
 * \param size Size of the array.
 * \param to_use Number of most recent values to use in calculation.
 */
static uint8_t avgArray(uint8_t array[], uint8_t size, uint8_t to_use)
{
    uint16_t sum = 0;
    for (uint8_t i = size - to_use; i < size; i++)
        sum += array[i];
    return (uint8_t)(sum / to_use);
}

/** intToString 
 * \brief Return the length of string.
 * \param str String to be measured.
 * \return The length of string..
 */
static uint8_t lenStr (unsigned char *str)
{
    uint8_t len = 0;
    while (*(str++) != '\0') len++;
    return len;
}

/** printFormat
 * \brief Print argument according to format.
 * \param format Character string composed of zero or more directives and the
 * characters to be print.
 * \param args A void * array, tha can point to diferents tipes values.
 * \example
 * char* name = "John";
 * uint8_t age = 55;
 * 
 * void** args = (void**)pvPortMalloc(2 * sizeof(void *));
 * args[0] = name;
 * args[1] = (uint8_t *) &age;
 * 
 * printFormat("Hello world! My name is %s and my age is %d\r\n", args);
 */
static void printFormat (const char * format, void **args ) 
{
    uint8_t arg_index = 0;
    while (*format != '\0')
    {
        if (*format != '%')
        {
            UARTCharPut(UART0_BASE, *(format++));
            continue;
        }

        format++;

        uint8_t count = 0;
        while (*format != 'd' && 
                *format != 'h' && 
                *format != 'c' && 
                *format != 's' && 
                *format != '\0' && 
                *format != '%') 
        {
            count++;
            format++;
        }

        uint8_t spaces = 0;
        tBoolean spaces_in_left = false;
    
        if (count != 0) 
        {
            if (*(format - count) == '-')
            {   
                spaces_in_left = true;
                count -= 1;
            }

            uint8_t multiplier = 1;
            for (uint8_t i = 1; i <= count; i++)
            {
                spaces += (*(format - i) - '0') * multiplier;
                multiplier *= 10;
            }
        }

        char *str = NULL;
        switch (*format)
        {
            case 'c':
                str = intToString(*((uint8_t *) args[arg_index++]));
                break;
            case 'h':
                str = intToString(*((uint16_t *) args[arg_index++]));
                break;
            case 'd':
                str = intToString(*((uint32_t *) args[arg_index++]));
                break;
            case 's':
                str = (char *) args[arg_index++];
                break;
            case '%':
                UARTCharPut(UART0_BASE, '%');
                break;
            case '\0':
                format--;
                break;
        }

        if (str != NULL) {
            char *str_to_print = str;
            uint8_t len_str = lenStr(str_to_print);

            spaces = spaces >= len_str ? spaces - len_str : 0;

            if (spaces_in_left) 
            {
                for (uint8_t i = 0; i < spaces; i++) 
                    UARTCharPut(UART0_BASE, ' ');
            }

            while (*str_to_print != '\0') 
                UARTCharPut(UART0_BASE, *(str_to_print++));

            if (!spaces_in_left) 
            {
                for (uint8_t i = 0; i < spaces; i++) 
                    UARTCharPut(UART0_BASE, ' ');
            }

            if (*format != 's') {
                vPortFree(str);
            }
        }

        format++;
    }
}

/** ulGetRunTimeCounterValue
 * \return The overflow timer counter 's_overflow_counter'.
 */
unsigned long ulGetRunTimeCounterValue ( void ) {
    return s_overflow_counter;
}

/* INTERRUPTS HANDLERS */
/** vUART_ISR 
 * \brief When a character comes form UART this handler interpretes the correct
 * to act. This handler can changes the value of N and can suspend and resume
 * the 'Top Task'.
 */
void vUART_ISR(void)
{
    /* Get UART status and clear flags */
    uint32_t status = UARTIntStatus(UART0_BASE, true);
    UARTIntClear(UART0_BASE, status); 

    /* Get all new characters coming */
    while (UARTCharsAvail(UART0_BASE)) {
        char c = UARTCharGet(UART0_BASE);

        /* If 'Top Task' is active and a 'q' comes, suspend 'Top Task'*/
        if (s_is_top_running)
        {
            if (c == 'q') 
            {
                vTaskSuspend(xTopTask);
                s_is_top_running = false;
                printString("Top Task was stopped\r\n");
            }
            continue;
        }

        /* If a character diferent to '\n' and '\r' comes, save it into the
         * buffer and increse the 's_uart_index' */
        if (c != '\n' && c != '\r') 
        { 
            UARTCharPut(UART0_BASE, c);
            if (s_uart_index > 2) break;
            s_uart_buffer[s_uart_index++] = c;
            continue;
        }

        /* None comand use only one character. So clear the buffer */
        if (s_uart_index == 1) {
            printString("\r\nInvalid command\r\n");
            s_uart_buffer[0] = '\0';
            s_uart_buffer[1] = '\0';
            s_uart_buffer[2] = '\0';
            s_uart_index = 0;
            continue;
        }

        /* When '\n' or '\r' arrives and the command is at least 2 character
         * length, will be analize. Commands can be 'top', 'Nx' and 'Nxx' with
         * x in [0-9] and 0 > Nxx <= MAX_NUMBER_OF_SAMPLES */
        if ((c == '\n' || c == '\r') && s_uart_index > 1)
        {
            switch (s_uart_buffer[0])
            {
            case 't':
                /* If command starts with 't' and its not 'top', is and invalid
                 * command. Otherwise its 'top' and 'Top Task' will be active */
                if (s_uart_buffer[1] == 'o' && s_uart_buffer[2] == 'p') 
                { 
                    vTaskResume(xTopTask);
                    s_is_top_running = true;
                    break;
                }
                printString("\r\nInvalid command");
                break;
            case 'N':
                /* If command starts with 'N' and have 3 characters,
                 * s_uart_buffer[2] must be a numbner */
                if (s_uart_index == 3 
                    && (s_uart_buffer[2] < '0' || s_uart_buffer[2] > '9')) 
                {
                    printString("\r\nInvalid command, N must be a number");
                    break;
                }
                /* If command starts with 'N' and have at least 2 characters,
                 * s_uart_buffer[1] must be a numbner */
                if (s_uart_index >= 2 && 
                    (s_uart_buffer[1] < '0' || s_uart_buffer[1] > '9')) 
                {
                    printString("\r\nInvalid command, N must be a number");
                    break;
                }
                
                /* Convert the numbner next to N in the command into the new N
                 * value */
                s_numberOfSamples = s_uart_buffer[1] - '0';
                if (s_uart_index == 3)
                    s_numberOfSamples = s_numberOfSamples * 10 + (s_uart_buffer[2] - '0');

                /* Verify the limits of N */
                if (s_numberOfSamples > MAX_NUMBER_OF_SAMPLES) 
                    s_numberOfSamples = MAX_NUMBER_OF_SAMPLES;
                if (s_numberOfSamples < MIN_NUMBER_OF_SAMPLES) 
                    s_numberOfSamples = MIN_NUMBER_OF_SAMPLES;
                break;
            default:
                printString("\r\nInvalid command");
                break;
            }

            /* Clear the buffer and print an endline */
            s_uart_buffer[0] = '\0';
            s_uart_buffer[1] = '\0';
            s_uart_buffer[2] = '\0';
            s_uart_index = 0;

            UARTCharPut(UART0_BASE, '\r');
            UARTCharPut(UART0_BASE, '\n');
        }
    }
}

/** vTimer0A_Handler 
 * \brief Clear the interrpt flag for Timer0A and increse the overflow timer
 * counter 's_overflow_counter'.
 */
void vTimer0A_Handler(void) {
    TimerIntClear(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
    s_overflow_counter++;
}