# rex2wav

A utility to convert Propellerheads Recycle REX (rx2) files into component slices, and extract information about the slice timing. Mac only, sorry.

## Build Instructions

1. Download the REX SDK from [Reason Studios Downloads](https://developer.reasonstudios.com/downloads/other-products).
2. Place the REX SDK in `~/SDKs/REXSDK_Mac_1.9.2`.
3. Build the project using:
   ```bash
   make
   ```

## Run Instructions

1. In the project directory, copy the `REX Shared Library.framework` into a directory called `Frameworks`:
   ```bash
   cp -R ~/SDKs/REXSDK_Mac_1.9.2/Mac/Deployment/REX\ Shared\ Library.framework Frameworks/
   ```

2. After this step, your directory structure should look like:
   ```
   bin/rex2wav
   Frameworks/REX Shared Library.framework
   ```

3. Run the program using:
   ```bash
   ./bin/rex2wav input.rx2
   ```

## Output

- The slices will be saved in a folder named `slices`.
- A CSV file containing the slice points and durations will also be generated.

You can also export the entire loop as a single file, alongside its slice points csv with 
   ```bash
   ./bin/rex2wav input.rx2 --single-file
   ```
