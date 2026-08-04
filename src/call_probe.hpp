#pragma once

// Watches a list of functions and reports who calls them.
//
// Static reading has repeatedly picked functions that look like the right ones
// and are not: four separate auto-chip queries were patched, verified applied,
// and changed nothing, because the widget consults none of them. The readout is
// drawn by a function with no direct callers and no vtable slot, so its caller
// cannot be found by reading at all.
//
// This arms a breakpoint on each address and records the return address of
// every call. Each distinct caller is reported once, so a per-frame draw costs
// a couple of lines rather than thousands. Recording happens in the handler and
// logging happens on the polling thread, because the interrupted thread may
// hold the very locks the logger needs.
bool install_call_probe();

// Drains what the handler recorded. Returns false when there is nothing new.
bool pop_call_site(const char*& label, unsigned long long& caller_rva);
