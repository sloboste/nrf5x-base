/* Copyright (c) 2015 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

#include "app_gpiote.h"
#include <stdlib.h>
#include <string.h>
#include "app_util.h"

#include "nrf_drv_gpiote.h"
#include "nrf_drv_common.h"
#include "nrf_drv_config.h"
#include "app_util_platform.h"
#include "nrf_assert.h"

#include "nrf_error.h"
#include "nrf_gpio.h"

#include "nrf_drv_common.h"

#define FORBIDDEN_HANDLER_ADDRESS ((nrf_drv_gpiote_evt_handler_t)UINT32_MAX)
#define PIN_NOT_USED              (-1)
#define PIN_USED                  (-2)
#define NO_CHANNELS               (-1)
#define SENSE_FIELD_POS           (6)
#define SENSE_FIELD_MASK          (0xC0)

/**
 * @brief Macro for conveting task-event index to an address of an event register.
 *
 * Macro utilizes the fact that registers are grouped together in ascending order.
 */
#define TE_IDX_TO_EVENT_ADDR(idx)   (nrf_gpiote_events_t)((uint32_t)NRF_GPIOTE_EVENTS_IN_0+(sizeof(uint32_t)*(idx)))

/**
 * @brief Macro for conveting task-event index to an address of a task register.
 *
 * Macro utilizes the fact that registers are grouped together in ascending order.
 */
#define TE_IDX_TO_TASK_ADDR(idx)   (nrf_gpiote_tasks_t)((uint32_t)NRF_GPIOTE_TASKS_OUT_0+(sizeof(uint32_t)*(idx)))

//lint -save -e661
typedef struct
{
    nrf_drv_gpiote_evt_handler_t handlers[NUMBER_OF_GPIO_TE+GPIOTE_CONFIG_NUM_OF_LOW_POWER_EVENTS];
    int8_t                       pin_assignments[NUMBER_OF_PINS];
    int8_t                       port_handlers_pins[GPIOTE_CONFIG_NUM_OF_LOW_POWER_EVENTS];
    nrf_drv_state_t              state;
} control_block_t;

/**@brief GPIOTE user type. */
typedef struct
{
    uint32_t                   pins_mask;             /**< Mask defining which pins user wants to monitor. */
    uint32_t                   pins_low_to_high_mask; /**< Mask defining which pins will generate events to this user when toggling low->high. */
    uint32_t                   pins_high_to_low_mask; /**< Mask defining which pins will generate events to this user when toggling high->low. */
    uint32_t                   sense_high_pins;       /**< Mask defining which pins are configured to generate GPIOTE interrupt on transition to high level. */
    app_gpiote_event_handler_t event_handler;         /**< Pointer to function to be executed when an event occurs. */
} gpiote_user_t;

STATIC_ASSERT(sizeof(gpiote_user_t) <= GPIOTE_USER_NODE_SIZE);
STATIC_ASSERT(sizeof(gpiote_user_t) % 4 == 0);

static uint32_t        m_enabled_users_mask;          /**< Mask for tracking which users are enabled. */
static uint8_t         m_user_array_size;             /**< Size of user array. */
static uint8_t         m_user_count;                  /**< Number of registered users. */
static gpiote_user_t * mp_users = NULL;               /**< Array of GPIOTE users. */

/**@brief Function for toggling sense level for specified pins.
 *
 * @param[in]   p_user   Pointer to user structure.
 * @param[in]   pins     Bitmask specifying for which pins the sense level is to be toggled.
 */
static void sense_level_toggle(gpiote_user_t * p_user, uint32_t pins)
{
    uint32_t pin_no;

    for (pin_no = 0; pin_no < NO_OF_PINS; pin_no++)
    {
        uint32_t pin_mask = (1 << pin_no);

        if ((pins & pin_mask) != 0)
        {
            uint32_t sense;

            // Invert sensing.
            if ((p_user->sense_high_pins & pin_mask) == 0)
            {
                sense                    = GPIO_PIN_CNF_SENSE_High << GPIO_PIN_CNF_SENSE_Pos;
                p_user->sense_high_pins |= pin_mask;
            }
            else
            {
                sense                    = GPIO_PIN_CNF_SENSE_Low << GPIO_PIN_CNF_SENSE_Pos;
                p_user->sense_high_pins &= ~pin_mask;
            }

            NRF_GPIO->PIN_CNF[pin_no] &= ~GPIO_PIN_CNF_SENSE_Msk;
            NRF_GPIO->PIN_CNF[pin_no] |= sense;
        }
    }
}


static void sense_level_disable(uint32_t pins)
{
    uint32_t pin_no;

    for (pin_no = 0; pin_no < 32; pin_no++)
    {
        uint32_t pin_mask = (1 << pin_no);

        if ((pins & pin_mask) != 0)
        {
            NRF_GPIO->PIN_CNF[pin_no] &= ~GPIO_PIN_CNF_SENSE_Msk;
            NRF_GPIO->PIN_CNF[pin_no] |= GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos;
        }
    }
}



// /**@brief Function for handling the GPIOTE interrupt.
//  */
// void GPIOTE_IRQHandler(void)
// {
//     uint8_t  i;
//     uint32_t pins_changed        = 1;
//     uint32_t pins_sense_enabled  = 0;
//     uint32_t pins_sense_disabled = 0;
//     uint32_t pins_state          = NRF_GPIO->IN;

//     // Clear event.
//     NRF_GPIOTE->EVENTS_PORT = 0;

//     while (pins_changed)
//     {
//         // Check all users.
//         for (i = 0; i < m_user_count; i++)
//         {
//             gpiote_user_t * p_user = &mp_users[i];

//             // Check if user is enabled.
//             if (((1 << i) & m_enabled_users_mask) != 0)
//             {
//                 uint32_t transition_pins;
//                 uint32_t event_low_to_high = 0;
//                 uint32_t event_high_to_low = 0;

//                 pins_sense_enabled |= (p_user->pins_mask & ~pins_sense_disabled);

//                 // Find set of pins on which there has been a transition.
//                 transition_pins = (pins_state ^ ~p_user->sense_high_pins) & (p_user->pins_mask & ~pins_sense_disabled);

//                 sense_level_disable(transition_pins);
//                 pins_sense_disabled |= transition_pins;
//                 pins_sense_enabled  &= ~pins_sense_disabled;

//                 // Call user event handler if an event has occurred.
//                 event_high_to_low |= (~pins_state & p_user->pins_high_to_low_mask) & transition_pins;
//                 event_low_to_high |= (pins_state & p_user->pins_low_to_high_mask) & transition_pins;

//                 if ((event_low_to_high | event_high_to_low) != 0)
//                 {
//                     p_user->event_handler(event_low_to_high, event_high_to_low);
//                 }
//             }
//         }

//         // Second read after setting sense.
//         // Check if any pins with sense enabled have changed while serving this interrupt.
//         pins_changed = (NRF_GPIO->IN ^ pins_state) & pins_sense_enabled;
//         pins_state  ^= pins_changed;
//     }

//     // Now re-enabling sense on all pins that have sense disabled.
//     // Note: a new interrupt might fire immediatly.
//     for (i = 0; i < m_user_count; i++)
//     {
//         gpiote_user_t * p_user = &mp_users[i];

//         // Check if user is enabled.
//         if (((1 << i) & m_enabled_users_mask) != 0)
//         {
//             if (pins_sense_disabled & p_user->pins_mask)
//             {
//                 sense_level_toggle(p_user, pins_sense_disabled & p_user->pins_mask);
//             }
//         }
//     }
// }


/**@brief Function for sense disabling for all pins for specified user.
 *
 * @param[in]  user_id   User id.
 */
static void pins_sense_disable(app_gpiote_user_id_t user_id)
{
    uint32_t pin_no;

    for (pin_no = 0; pin_no < 32; pin_no++)
    {
        if ((mp_users[user_id].pins_mask & (1 << pin_no)) != 0)
        {
            NRF_GPIO->PIN_CNF[pin_no] &= ~GPIO_PIN_CNF_SENSE_Msk;
            NRF_GPIO->PIN_CNF[pin_no] |= GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos;
        }
    }
}


uint32_t app_gpiote_init(uint8_t max_users, void * p_buffer)
{
    if (p_buffer == NULL)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    // Check that buffer is correctly aligned.
    if (!is_word_aligned(p_buffer))
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    // Initialize file globals.
    mp_users             = (gpiote_user_t *)p_buffer;
    m_user_array_size    = max_users;
    m_user_count         = 0;
    m_enabled_users_mask = 0;

    memset(mp_users, 0, m_user_array_size * sizeof(gpiote_user_t));

    // Initialize GPIOTE interrupt (will not be enabled until app_gpiote_user_enable() is called).
    NRF_GPIOTE->INTENCLR = 0xFFFFFFFF;

    NVIC_ClearPendingIRQ(GPIOTE_IRQn);
    NVIC_SetPriority(GPIOTE_IRQn, APP_IRQ_PRIORITY_HIGH);
    NVIC_EnableIRQ(GPIOTE_IRQn);

    return NRF_SUCCESS;
}


uint32_t app_gpiote_user_register(app_gpiote_user_id_t     * p_user_id,
                                  uint32_t                   pins_low_to_high_mask,
                                  uint32_t                   pins_high_to_low_mask,
                                  app_gpiote_event_handler_t event_handler)
{
    // Check state and parameters.
    if (mp_users == NULL)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    if (event_handler == NULL)
    {
        return NRF_ERROR_INVALID_PARAM;
    }
    if (m_user_count >= m_user_array_size)
    {
        return NRF_ERROR_NO_MEM;
    }

    // Allocate new user.
    mp_users[m_user_count].pins_mask             = pins_low_to_high_mask | pins_high_to_low_mask;
    mp_users[m_user_count].pins_low_to_high_mask = pins_low_to_high_mask;
    mp_users[m_user_count].pins_high_to_low_mask = pins_high_to_low_mask;
    mp_users[m_user_count].event_handler         = event_handler;

    *p_user_id = m_user_count++;

    // Make sure SENSE is disabled for all pins.
    pins_sense_disable(*p_user_id);

    return NRF_SUCCESS;
}


uint32_t app_gpiote_user_enable(app_gpiote_user_id_t user_id)
{
    uint32_t pin_no;
    uint32_t pins_state;

    // Check state and parameters.
    if (mp_users == NULL)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    if (user_id >= m_user_count)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    // Clear any pending event.
    NRF_GPIOTE->EVENTS_PORT = 0;
    pins_state              = NRF_GPIO->IN;

    // Enable user.
    if (m_enabled_users_mask == 0)
    {
        NRF_GPIOTE->INTENSET = GPIOTE_INTENSET_PORT_Msk;
    }
    m_enabled_users_mask |= (1 << user_id);

    // Enable sensing for all pins for specified user.
    mp_users[user_id].sense_high_pins = 0;
    for (pin_no = 0; pin_no < 32; pin_no++)
    {
        uint32_t pin_mask = (1 << pin_no);

        if ((mp_users[user_id].pins_mask & pin_mask) != 0)
        {
            uint32_t sense;

            if ((pins_state & pin_mask) != 0)
            {
                sense = GPIO_PIN_CNF_SENSE_Low << GPIO_PIN_CNF_SENSE_Pos;
            }
            else
            {
                sense = GPIO_PIN_CNF_SENSE_High << GPIO_PIN_CNF_SENSE_Pos;
                mp_users[user_id].sense_high_pins |= pin_mask;
            }

            NRF_GPIO->PIN_CNF[pin_no] &= ~GPIO_PIN_CNF_SENSE_Msk;
            NRF_GPIO->PIN_CNF[pin_no] |= sense;
        }
    }

    return NRF_SUCCESS;
}


uint32_t app_gpiote_user_disable(app_gpiote_user_id_t user_id)
{
    // Check state and parameters.
    if (mp_users == NULL)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    if (user_id >= m_user_count)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    // Disable sensing for all pins for specified user.
    pins_sense_disable(user_id);

    // Disable user.
    m_enabled_users_mask &= ~(1UL << user_id);
    if (m_enabled_users_mask == 0)
    {
        NRF_GPIOTE->INTENCLR = GPIOTE_INTENSET_PORT_Msk;
    }

    return NRF_SUCCESS;
}


uint32_t app_gpiote_pins_state_get(app_gpiote_user_id_t user_id, uint32_t * p_pins)
{
    gpiote_user_t * p_user;

    // Check state and parameters.
    if (mp_users == NULL)
    {
        return NRF_ERROR_INVALID_STATE;
    }
    if (user_id >= m_user_count)
    {
        return NRF_ERROR_INVALID_PARAM;
    }

    // Get pins.
    p_user  = &mp_users[user_id];
    *p_pins = NRF_GPIO->IN & p_user->pins_mask;

    return NRF_SUCCESS;
}

#if defined(SVCALL_AS_NORMAL_FUNCTION) || defined(SER_CONNECTIVITY)
uint32_t app_gpiote_input_event_handler_register(const uint8_t                    channel,
                                                 const uint32_t                   pin,
                                                 const uint32_t                   polarity,
                                                 app_gpiote_input_event_handler_t event_handler)
{
    (void)sense_level_toggle(NULL, pin);
    return NRF_ERROR_NOT_SUPPORTED;
}

uint32_t app_gpiote_input_event_handler_unregister(const uint8_t channel)
{
    return NRF_ERROR_NOT_SUPPORTED;
}

uint32_t app_gpiote_end_irq_event_handler_register(app_gpiote_input_event_handler_t event_handler)
{
    return NRF_ERROR_NOT_SUPPORTED;
}

uint32_t app_gpiote_end_irq_event_handler_unregister(void)
{
    return NRF_ERROR_NOT_SUPPORTED;
}

uint32_t app_gpiote_enable_interrupts(void)
{
    return NRF_ERROR_NOT_SUPPORTED;
}

uint32_t app_gpiote_disable_interrupts(void)
{
    return NRF_ERROR_NOT_SUPPORTED;
}
#endif // SVCALL_AS_NORMAL_FUNCTION || SER_CONNECTIVITY


static control_block_t m_cb;

__STATIC_INLINE bool pin_in_use(uint32_t pin)
{
    return (m_cb.pin_assignments[pin] != PIN_NOT_USED);
}

__STATIC_INLINE bool pin_in_use_as_non_task_out(uint32_t pin)
{
    return (m_cb.pin_assignments[pin] == PIN_USED);
}

__STATIC_INLINE bool pin_in_use_by_te(uint32_t pin)
{
    return (m_cb.pin_assignments[pin] >= 0 && m_cb.pin_assignments[pin] < NUMBER_OF_GPIO_TE) ? true : false;
}

__STATIC_INLINE bool pin_in_use_by_port(uint32_t pin)
{
    return (m_cb.pin_assignments[pin] >= NUMBER_OF_GPIO_TE);
}

__STATIC_INLINE bool pin_in_use_by_gpiote(uint32_t pin)
{
    return (m_cb.pin_assignments[pin] >= 0);
}

__STATIC_INLINE void pin_in_use_by_te_set(uint32_t pin,
                                          uint32_t channel_id,
                                          nrf_drv_gpiote_evt_handler_t handler,
                                          bool is_channel)
{
    m_cb.pin_assignments[pin] = channel_id;
    m_cb.handlers[channel_id] = handler;
    if (!is_channel)
    {
        m_cb.port_handlers_pins[channel_id-NUMBER_OF_GPIO_TE] = (int8_t)pin;
    }
}

__STATIC_INLINE void pin_in_use_set(uint32_t pin)
{
    m_cb.pin_assignments[pin] = PIN_USED;
}

__STATIC_INLINE void pin_in_use_clear(uint32_t pin)
{
    m_cb.pin_assignments[pin] = PIN_NOT_USED;
}

__STATIC_INLINE int8_t channel_port_get(uint32_t pin)
{
    return m_cb.pin_assignments[pin];
}

__STATIC_INLINE nrf_drv_gpiote_evt_handler_t channel_handler_get(uint32_t channel)
{
    return m_cb.handlers[channel];
}

static int8_t channel_port_alloc(uint32_t pin,nrf_drv_gpiote_evt_handler_t handler, bool channel)
{
    int8_t channel_id = NO_CHANNELS;
    uint32_t i;

    uint32_t start_idx = channel ? 0 : NUMBER_OF_GPIO_TE;
    uint32_t end_idx = channel ? NUMBER_OF_GPIO_TE : (NUMBER_OF_GPIO_TE+GPIOTE_CONFIG_NUM_OF_LOW_POWER_EVENTS);
    //critical section

    for (i = start_idx; i < end_idx; i++)
    {
        if (m_cb.handlers[i] == FORBIDDEN_HANDLER_ADDRESS)
        {
            pin_in_use_by_te_set(pin, i, handler, channel);
            channel_id = i;
            break;
        }
    }
    //critical section
    return channel_id;
}

static void channel_free(uint8_t channel_id)
{
    m_cb.handlers[channel_id] = FORBIDDEN_HANDLER_ADDRESS;
    if (channel_id >= NUMBER_OF_GPIO_TE)
    {
        m_cb.port_handlers_pins[channel_id-NUMBER_OF_GPIO_TE] = (int8_t)PIN_NOT_USED;
    }
}

ret_code_t nrf_drv_gpiote_init(void)
{
    if (m_cb.state != NRF_DRV_STATE_UNINITIALIZED)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    uint8_t i;
    for (i = 0; i < NUMBER_OF_PINS; i++)
    {
        pin_in_use_clear(i);
    }
    for (i = 0; i < (NUMBER_OF_GPIO_TE+GPIOTE_CONFIG_NUM_OF_LOW_POWER_EVENTS); i++)
    {
        channel_free(i);
    }

    nrf_drv_common_irq_enable(GPIOTE_IRQn, GPIOTE_CONFIG_IRQ_PRIORITY);
    nrf_gpiote_int_enable(GPIOTE_INTENSET_PORT_Msk);
    m_cb.state = NRF_DRV_STATE_INITIALIZED;

    return NRF_SUCCESS;
}

bool nrf_drv_gpiote_is_init(void)
{
    return (m_cb.state != NRF_DRV_STATE_UNINITIALIZED) ? true : false;
}

void nrf_drv_gpiote_uninit(void)
{
    ASSERT(m_cb.state!=NRF_DRV_STATE_UNINITIALIZED);

    uint32_t i;
    for (i = 0; i < NUMBER_OF_PINS; i++)
    {
        if (pin_in_use_as_non_task_out(i))
        {
            nrf_drv_gpiote_out_uninit(i);
        }
        else if( pin_in_use_by_gpiote(i))
        {
            /* Disable gpiote_in is having the same effect on out pin as gpiote_out_uninit on
             * so it can be called on all pins used by GPIOTE.
             */
            nrf_drv_gpiote_in_uninit(i);
        }
    }
    m_cb.state = NRF_DRV_STATE_UNINITIALIZED;
}

ret_code_t nrf_drv_gpiote_out_init(nrf_drv_gpiote_pin_t pin, 
                                   nrf_drv_gpiote_out_config_t const * p_config)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(m_cb.state == NRF_DRV_STATE_INITIALIZED);
    ASSERT(p_config);

    ret_code_t result = NRF_SUCCESS;

    if (pin_in_use(pin))
    {
        result = NRF_ERROR_INVALID_STATE;
    }
    else
    {
        if (p_config->task_pin)
        {
            int8_t channel = channel_port_alloc(pin, NULL, true);

            if (channel != NO_CHANNELS)
            {
                nrf_gpiote_task_configure(channel, pin, p_config->action, p_config->init_state);
            }
            else
            {
                result = NRF_ERROR_NO_MEM;
            }
        }
        else
        {
            pin_in_use_set(pin);
        }

        if (result == NRF_SUCCESS)
        {
            nrf_gpio_cfg_output(pin);

            if (p_config->init_state == NRF_GPIOTE_INITIAL_VALUE_HIGH)
            {
                nrf_gpio_pin_set(pin);
            }
            else
            {
                nrf_gpio_pin_clear(pin);
            }
        }
    }

    return result;
}

void nrf_drv_gpiote_out_uninit(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use(pin));

    if (pin_in_use_by_te(pin))
    {
        channel_free((uint8_t)m_cb.pin_assignments[pin]);
    }
    pin_in_use_clear(pin);

    nrf_gpio_cfg_default(pin);
}

void nrf_drv_gpiote_out_set(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use(pin));
    ASSERT(!pin_in_use_by_te(pin))

    nrf_gpio_pin_set(pin);
}

void nrf_drv_gpiote_out_clear(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use(pin));
    ASSERT(!pin_in_use_by_te(pin))

    nrf_gpio_pin_clear(pin);
}

void nrf_drv_gpiote_out_toggle(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use(pin));
    ASSERT(!pin_in_use_by_te(pin))

    nrf_gpio_pin_toggle(pin);
}

void nrf_drv_gpiote_out_task_enable(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use(pin));
    ASSERT(pin_in_use_by_te(pin))

    nrf_gpiote_task_enable(m_cb.pin_assignments[pin]);
}

void nrf_drv_gpiote_out_task_disable(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use(pin));
    ASSERT(pin_in_use_by_te(pin))

    nrf_gpiote_task_disable(m_cb.pin_assignments[pin]);
}

uint32_t nrf_drv_gpiote_out_task_addr_get(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use_by_te(pin));
    
    nrf_gpiote_tasks_t task = TE_IDX_TO_TASK_ADDR(channel_port_get(pin));
    return nrf_gpiote_task_addr_get(task);
}

void nrf_drv_gpiote_out_task_force(nrf_drv_gpiote_pin_t pin, uint8_t state)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use(pin));
    ASSERT(pin_in_use_by_te(pin));
    
    nrf_gpiote_outinit_t init_val = state ? NRF_GPIOTE_INITIAL_VALUE_HIGH : NRF_GPIOTE_INITIAL_VALUE_LOW;
    nrf_gpiote_task_force(m_cb.pin_assignments[pin], init_val);
}

void nrf_drv_gpiote_out_task_trigger(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use(pin));
    ASSERT(pin_in_use_by_te(pin));

    nrf_gpiote_tasks_t task = TE_IDX_TO_TASK_ADDR(channel_port_get(pin));;
    nrf_gpiote_task_set(task);
}

ret_code_t nrf_drv_gpiote_in_init(nrf_drv_gpiote_pin_t pin,
                                  nrf_drv_gpiote_in_config_t const * p_config,
                                  nrf_drv_gpiote_evt_handler_t evt_handler)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ret_code_t result = NRF_SUCCESS;
    /* Only one GPIOTE channel can be assigned to one physical pin. */
    if (pin_in_use_by_gpiote(pin))
    {
        result = NRF_ERROR_INVALID_STATE;
    }
    else
    {
        int8_t channel = channel_port_alloc(pin, evt_handler, p_config->hi_accuracy);
        if (channel != NO_CHANNELS)
        {
            if (p_config->is_watcher)
            {
                nrf_gpio_cfg_watcher(pin);
            }
            else
            {
                nrf_gpio_cfg_input(pin,p_config->pull);
            }

            if (p_config->hi_accuracy)
            {
                nrf_gpiote_event_configure(channel, pin,p_config->sense);
            }
            else
            {
                m_cb.port_handlers_pins[channel-NUMBER_OF_GPIO_TE] |= (p_config->sense)<< SENSE_FIELD_POS;
            }
        }
        else
        {
            result = NRF_ERROR_NO_MEM;
        }
    }
    return result;
}

void nrf_drv_gpiote_in_event_enable(nrf_drv_gpiote_pin_t pin, bool int_enable)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use_by_gpiote(pin));
    if (pin_in_use_by_port(pin))
    {
        uint8_t pin_and_sense = m_cb.port_handlers_pins[channel_port_get(pin)-NUMBER_OF_GPIO_TE];
        nrf_gpiote_polarity_t polarity = (nrf_gpiote_polarity_t)(pin_and_sense >> SENSE_FIELD_POS);
        nrf_gpio_pin_sense_t sense;
        if (polarity == NRF_GPIOTE_POLARITY_TOGGLE)
        {
            /* read current pin state and set for next sense to oposit */
            sense = (nrf_gpio_pins_read() & (1 << pin)) ?
                    NRF_GPIO_PIN_SENSE_LOW : NRF_GPIO_PIN_SENSE_HIGH;
        }
        else
        {
            sense = (polarity == NRF_GPIOTE_POLARITY_LOTOHI) ?
                    NRF_GPIO_PIN_SENSE_HIGH : NRF_GPIO_PIN_SENSE_LOW;
        }
        nrf_gpio_cfg_sense_set(pin,sense);
    }
    else if(pin_in_use_by_te(pin))
    {
        int32_t channel = (int32_t)channel_port_get(pin);
        nrf_gpiote_events_t event = TE_IDX_TO_EVENT_ADDR(channel);
       
        nrf_gpiote_event_enable(channel);

        nrf_gpiote_event_clear(event);
        if (int_enable)
        {
            nrf_drv_gpiote_evt_handler_t handler = channel_handler_get(channel_port_get(pin));
            // Enable the interrupt only if event handler was provided.
            if (handler)
            {
                nrf_gpiote_int_enable(1 << channel);
            }
        }
    }
}

void nrf_drv_gpiote_in_event_disable(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use_by_gpiote(pin));
    if (pin_in_use_by_port(pin))
    {
        nrf_gpio_input_disconnect(pin);
    }
    else if(pin_in_use_by_te(pin))
    {
        int32_t channel = (int32_t)channel_port_get(pin);
        nrf_gpiote_event_disable(channel);
        nrf_gpiote_int_disable(1 << channel);
    }
}

void nrf_drv_gpiote_in_uninit(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use_by_gpiote(pin));
    nrf_drv_gpiote_in_event_disable(pin);
    if(pin_in_use_by_te(pin))
    {
        nrf_gpiote_te_default(channel_port_get(pin));
    }
    nrf_gpio_cfg_default(pin);
    channel_free((uint8_t)channel_port_get(pin));
    pin_in_use_clear(pin);
}

bool nrf_drv_gpiote_in_is_set(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    return nrf_gpio_pin_read(pin) ? true : false;
}

uint32_t nrf_drv_gpiote_in_event_addr_get(nrf_drv_gpiote_pin_t pin)
{
    ASSERT(pin < NUMBER_OF_PINS);
    ASSERT(pin_in_use_by_te(pin));
    
    nrf_gpiote_events_t event = TE_IDX_TO_EVENT_ADDR(channel_port_get(pin));
    return nrf_gpiote_event_addr_get(event);
}

void GPIOTE_IRQHandler(void)
{
    uint32_t status = 0;
    uint32_t input = 0;

    if(NRF_GPIOTE->EVENTS_PORT != 0)
    {
        uint8_t  i;
        uint32_t pins_changed        = 1;
        uint32_t pins_sense_enabled  = 0;
        uint32_t pins_sense_disabled = 0;
        uint32_t pins_state          = NRF_GPIO->IN;

        // Clear event.
        NRF_GPIOTE->EVENTS_PORT = 0;

        while (pins_changed)
        {
            // Check all users.
            for (i = 0; i < m_user_count; i++)
            {
                gpiote_user_t * p_user = &mp_users[i];

                // Check if user is enabled.
                if (((1 << i) & m_enabled_users_mask) != 0)
                {
                    uint32_t transition_pins;
                    uint32_t event_low_to_high = 0;
                    uint32_t event_high_to_low = 0;

                    pins_sense_enabled |= (p_user->pins_mask & ~pins_sense_disabled);

                    // Find set of pins on which there has been a transition.
                    transition_pins = (pins_state ^ ~p_user->sense_high_pins) & (p_user->pins_mask & ~pins_sense_disabled);

                    sense_level_disable(transition_pins);
                    pins_sense_disabled |= transition_pins;
                    pins_sense_enabled  &= ~pins_sense_disabled;

                    // Call user event handler if an event has occurred.
                    event_high_to_low |= (~pins_state & p_user->pins_high_to_low_mask) & transition_pins;
                    event_low_to_high |= (pins_state & p_user->pins_low_to_high_mask) & transition_pins;

                    if ((event_low_to_high | event_high_to_low) != 0)
                    {
                        p_user->event_handler(event_low_to_high, event_high_to_low);
                    }
                }
            }

            // Second read after setting sense.
            // Check if any pins with sense enabled have changed while serving this interrupt.
            pins_changed = (NRF_GPIO->IN ^ pins_state) & pins_sense_enabled;
            pins_state  ^= pins_changed;
        }

        // Now re-enabling sense on all pins that have sense disabled.
        // Note: a new interrupt might fire immediatly.
        for (i = 0; i < m_user_count; i++)
        {
            gpiote_user_t * p_user = &mp_users[i];

            // Check if user is enabled.
            if (((1 << i) & m_enabled_users_mask) != 0)
            {
                if (pins_sense_disabled & p_user->pins_mask)
                {
                    sense_level_toggle(p_user, pins_sense_disabled & p_user->pins_mask);
                }
            }
        }
    }
    else
    {
        /* collect status of all GPIOTE pin events. Processing is done once all are collected and cleared.*/
        uint32_t i;
        nrf_gpiote_events_t event = NRF_GPIOTE_EVENTS_IN_0;
        uint32_t mask = (uint32_t)NRF_GPIOTE_INT_IN0_MASK;
        for (i = 0; i < NUMBER_OF_GPIO_TE; i++)
        {
            if (nrf_gpiote_event_is_set(event) && nrf_gpiote_int_is_enabled(mask))
            {
                nrf_gpiote_event_clear(event);
                status |= mask;
            }
            mask <<= 1;
            /* Incrementing to next event, utilizing the fact that events are grouped together
             * in ascending order. */
            event = (nrf_gpiote_events_t)((uint32_t)event + sizeof(uint32_t));
        }

        /* collect PORT status event, if event is set read pins state. Processing is postponed to the
         * end of interrupt. */
        if (nrf_gpiote_event_is_set(NRF_GPIOTE_EVENTS_PORT))
        {
            nrf_gpiote_event_clear(NRF_GPIOTE_EVENTS_PORT);
            status |= (uint32_t)NRF_GPIOTE_INT_PORT_MASK;
            input = nrf_gpio_pins_read();
        }

        /* Process pin events. */
        if (status & NRF_GPIOTE_INT_IN_MASK)
        {
            mask = (uint32_t)NRF_GPIOTE_INT_IN0_MASK;
            for (i = 0; i < NUMBER_OF_GPIO_TE; i++)
            {
                if (mask & status)
                {
                    nrf_drv_gpiote_pin_t pin = nrf_gpiote_event_pin_get(i);
                    nrf_gpiote_polarity_t polarity = nrf_gpiote_event_polarity_get(i);
                    nrf_drv_gpiote_evt_handler_t handler = channel_handler_get(i);
                    handler(pin,polarity);
                }
                mask <<= 1;
            }
        }

        if (status & (uint32_t)NRF_GPIOTE_INT_PORT_MASK)
        {
            /* Process port event. */
            for (i = 0; i < GPIOTE_CONFIG_NUM_OF_LOW_POWER_EVENTS; i++)
            {
                if (m_cb.port_handlers_pins[i] != PIN_NOT_USED)
                {
                    uint8_t pin_and_sense = m_cb.port_handlers_pins[i];
                    nrf_drv_gpiote_pin_t pin = (pin_and_sense & ~SENSE_FIELD_MASK);
                    nrf_drv_gpiote_evt_handler_t handler = channel_handler_get(channel_port_get(pin));
                    if (handler)
                    {
                        nrf_gpiote_polarity_t polarity =
                                (nrf_gpiote_polarity_t)((pin_and_sense & SENSE_FIELD_MASK) >> SENSE_FIELD_POS);
                        mask = 1 << pin;
                        nrf_gpio_pin_sense_t sense = nrf_gpio_pin_sense_get(pin);
                        if (((mask & input) && (sense==NRF_GPIO_PIN_SENSE_HIGH)) ||
                           (!(mask & input) && (sense==NRF_GPIO_PIN_SENSE_LOW))  )
                        {
                            if (polarity == NRF_GPIOTE_POLARITY_TOGGLE)
                            {
                                nrf_gpio_pin_sense_t next_sense = (sense == NRF_GPIO_PIN_SENSE_HIGH) ?
                                        NRF_GPIO_PIN_SENSE_LOW : NRF_GPIO_PIN_SENSE_HIGH;
                                nrf_gpio_cfg_sense_set(pin, next_sense);
                            }
                            handler(pin, polarity);
                        }
                    }
                }
            }
        }
    }
}
//lint -restore
