# Description
Arbitrary physical memory read/write using Toshiba qiomem.sys from ring 3 (only works upto 4GB physical addresses).\
[Driver](https://www.catalog.update.microsoft.com/ScopedViewInline.aspx?updateid=b2b6cde4-486b-4665-9093-19bf582b86fd) is distributed via Microsoft Update Catalog. Other [variants](https://www.catalog.update.microsoft.com/Search.aspx?q=Generic%20IO%20%26%20Memory%20Access). Not listed on Microsoft Vulnerable Driver List, no previous CVE alloted.

first run `acpi.exe` to create the fake device and then `main.exe`.
