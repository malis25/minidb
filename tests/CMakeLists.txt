# Keep executable smoke tests and core-library unit tests in their own directories.
add_subdirectory(unit)

# The application's version flag must work without opening a listener or database.
add_test(NAME minidb_cli_version COMMAND minidb --version)
