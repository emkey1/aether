#ifndef ISH_DIAGNOSTICS_BRIDGE_H
#define ISH_DIAGNOSTICS_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

void ISHDiagnosticsRecordGuestFatalSync(const char *kind, const char *summary, const char *detail);
void ISHDiagnosticsRecordGuestExitSyncDetailed(int pid, int ppid, int tgid, const char *comm, int code);

#ifdef __cplusplus
}
#endif

#endif
