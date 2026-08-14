# Microsoft Project desktop oracle

Configure ScheduleIO with `-DSCHEDULEIO_ORACLE_MSPROJECT=ON` on a Windows
machine where Microsoft Project desktop is installed, then run:

```powershell
ctest --test-dir build -R tst_msproject_usage_view --output-on-failure
```

The test writes native Task Usage and Resource Usage table widths and timescale
sizes, launches Microsoft Project through COM, verifies the table widths through
Project's object model, performs a native Project resave, and asks ScheduleIO to
reopen that resaved file and verify all values again. Enabling the option makes
the installed Microsoft Project instance a required test dependency; a missing
COM registration is a test failure rather than a skip.
