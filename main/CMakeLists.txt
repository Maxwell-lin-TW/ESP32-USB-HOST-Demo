file(GLOB project_objects
    # CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)

idf_component_register(SRCS ${project_objects}
                    INCLUDE_DIRS ".")
