@echo off
rem envy-managed schema "@@ENVY_PRODUCT_SCRIPT_VERSION@@"
rem The sibling launcher injects --project, so envy resolves the project this script was
rem deployed into. `product --run` installs the tool if needed, activates the project
rem unless the caller already is its current activation, and runs the tool in that
rem environment, passing its exit code back.
call "%~dp0envy.bat" product --run "@@PRODUCT_NAME@@" -- %*
exit /b %ERRORLEVEL%
