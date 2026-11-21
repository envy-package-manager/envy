@echo off
rem envy-managed schema "@@ENVY_PRODUCT_SCRIPT_VERSION@@"
rem No `call`: envy.bat takes over, so carets aren't doubled and its exit code is ours.
"%~dp0envy.bat" product --run "@@PRODUCT_NAME@@" -- %*
