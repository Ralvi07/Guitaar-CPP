# Guitaar C++

Guitaar C++ is the native Windows continuation of [Guitaar](https://github.com/Ralvi07/Guitaar), the original JavaFX inventory management coursework.

- **Articles** tracks barcodes, stock, purchase and selling prices, discounts, and quantities sold.
- **Categories** and **Suppliers** manage pricing rules. Articles can also use a custom markup or fixed price.
- **Sectors** groups categories and employees. Separate multiple names with semicolons.
- **Checkout** creates a receipt by barcode. Completing a sale updates stock and saves the receipt. **Receipts** lets you export a saved sale again.

## Run on Windows

Download **Guitaar.exe** from [Releases](https://github.com/Ralvi07/Guitaar-CPP/releases) and open it on Windows x64. The app opens directly to the inventory and saves changes automatically. **File → Open data folder** opens the inventory and receipt folder.

To bring over your coursework data, select **File → Import Java inventory** and open the original storage.dat. Import replaces your current C++ inventory after confirmation and preserves imported selling prices as fixed prices. You can select automatic or custom pricing for each article afterward.

## Build from source

Run these commands in the MSYS2 UCRT64 shell with its GCC and binutils packages installed.

    windres guitaar.rc icon.o
    g++ -std=c++20 -O2 -municode -mwindows -static Guitaar.cpp icon.o -o Guitaar.exe -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -lgdi32
