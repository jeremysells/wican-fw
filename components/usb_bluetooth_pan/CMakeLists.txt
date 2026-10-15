set(pan_srcs usb_bluetooth_pan.c pan_policy.c pan_settings.c pan_http.c)
set(pan_includes include .)
set(pan_embeds web/usb-pan.js)

if(CONFIG_WICAN_USB_BLUETOOTH_PAN)
    # An external controller uses an independent Classic host. IDF's existing
    # NimBLE host and onboard BLE controller are not reconfigured.
    set(btstack_root "$ENV{WICAN_BTSTACK_PATH}")
    if(NOT btstack_root)
        include(FetchContent)
        FetchContent_Declare(wican_btstack
            GIT_REPOSITORY https://github.com/bluekitchen/btstack.git
            GIT_TAG 075a0780f0fad7ff67d58ac19f46e8953656a752)
        FetchContent_GetProperties(wican_btstack)
        if(NOT wican_btstack_POPULATED)
            FetchContent_Populate(wican_btstack)
        endif()
        set(btstack_root "${wican_btstack_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${btstack_root}/src/classic/bnep.c")
        message(FATAL_ERROR "WICAN_BTSTACK_PATH must contain BTstack v1.8.2")
    endif()
    list(APPEND pan_srcs pan_usb.c pan_backend.c)
    foreach(src btstack_linked_list btstack_linked_queue btstack_memory
                btstack_memory_pool btstack_run_loop btstack_util btstack_crypto
                btstack_tlv hci hci_cmd hci_dump l2cap l2cap_signaling ad_parser)
        list(APPEND pan_srcs "${btstack_root}/src/${src}.c")
    endforeach()
    foreach(src bnep sdp_client sdp_util)
        list(APPEND pan_srcs "${btstack_root}/src/classic/${src}.c")
    endforeach()
    list(APPEND pan_srcs "${btstack_root}/platform/embedded/btstack_run_loop_embedded.c")
    list(APPEND pan_includes "${btstack_root}/src" "${btstack_root}/platform/embedded")
endif()

if(CONFIG_WICAN_WEBUI_V2)
    idf_build_get_property(python PYTHON)
    set(pan_index "${CMAKE_CURRENT_BINARY_DIR}/pan_index.html.gz")
    set(ui_root "${MEATPI_COMPONENTS_PATH}/web_ui_v2")
    add_custom_command(OUTPUT "${pan_index}"
        COMMAND "${python}" "${CMAKE_CURRENT_LIST_DIR}/tools/build_ui.py"
                "${ui_root}" "${pan_index}"
        DEPENDS "${ui_root}/web/index.html" "${ui_root}/gzip_asset.py"
                "${CMAKE_CURRENT_LIST_DIR}/tools/build_ui.py"
                "${CMAKE_CURRENT_LIST_DIR}/web/usb-pan.js"
        VERBATIM)
    add_custom_target(usb_pan_ui DEPENDS "${pan_index}")
    list(APPEND pan_embeds "${pan_index}")
endif()

idf_component_register(SRCS ${pan_srcs} INCLUDE_DIRS ${pan_includes}
    PRIV_REQUIRES settings_manager log_manager cmdline_manager
                  dev_status_manager http_server_manager usb_host_manager
                  cherryusb esp_netif esp_timer nvs_flash lwip esp_http_server
    EMBED_FILES ${pan_embeds})
if(CONFIG_WICAN_WEBUI_V2)
    add_dependencies(${COMPONENT_LIB} usb_pan_ui)
endif()
if(CONFIG_WICAN_USB_BLUETOOTH_PAN)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u usb_pan_class_info")
endif()
