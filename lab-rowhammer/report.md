## 1-2

**In a 64-bit system using 4KB pages, which bits are used to represent the page offset, and which are used to represent the page number?**

**How about for a 64-bit system using 2MB pages? Which bits are used for page number and which are for page offset?**

**In a 2GB buffer, how many 2MB hugepages are there?**

4KB pages: bits 0-11 are page offset (12 bits), bits 12-63 are page number
2MB pages: bits 0-20 are page offset (21 bits), bits 21-63 are page number
2GB / 2MB = 1024 hugepages

## 2-1

**Given a victim address 0x96ec3000, what is the value of its Row id? The value of its Column id?**

**For the same address, assume an arbitrary XOR function for computing the Bank id, list all possible attacker addresses whose Row id is one more than 0x96ec3000's Row id and all the other ids match, including the Bank id and Column id. Hint: there should be 16 such addresses total.**

0x96ee1000
0x96ee3000
0x96ee5000
0x96ee7000
0x96ee9000
0x96eeb000
0x96eed000
0x96eef000
0x96ef1000
0x96ef3000
0x96ef5000
0x96ef7000
0x96ef9000
0x96efb000
0x96efd000
0x96eff000

## 2-3

**Analyze the statistics produced by your code when running part2, and report a threshold to distinguish the bank conflict.**
The threshold was 400 cycles

## 3-2

**Based on the XOR function you reverse-engineered, determine which of the 16 candidate addresses you derived in Discussion Question 2-1 maps to the same bank.**
Using XOR bank function (function 0), we calculated the bank id of the target address and the bank's 16 candidate addresses. The 0x96ee7000 address produced the same bank id (6) as the target address; therefore, 0x96ee7000 is the candidate that is mapped to the same bank.

## 4-2

**The default data pattern in part4.cc is to set aggressor rows to all 1’s and victim row to all 0’s. Try different data pattern and include the bitflip observation statistics in the table below. Then answer the following questions:**

**Do your results match your expectations? What is the best pattern to trigger flips effectively?**
| Data Pattern (Victim/Aggressor) | 0x00/0xff | 0xff/0x00 | 0x00/0x00 | 0xff/0xff |
| Number of Flips (100 trials) | 93 | 94 | 91 | 92 |

The research findings indicate the likelihood of having many bit-flips regardless of which of the 4 patterns the data falls into. The results had less variation that expected because it was hypothesized that the most extreme opposite pairing of aggressor/victim data (0x00 paired with 0xff) would produce the highest number of flips because of the high level of contrast between the two data types. However, all 4 data types produced very similar results, as there were between 91-94 successful bit-flip tests out of 100 for each of the data types.

The pairing of data types (0xff/0x00) yielded the greatest number of successful test results with a total of 94 successful bit-flip tests out of 100. Because there was such a small number of difference in successful bit-flip tests between all patterns, we conclude that this machine would be highly susceptible to using all four data pairs for creating bit-flips. The pairing of data types (0xff/0x00) was the most successful during our testing.

## 5-1

**Given the ECC type descriptions listed above, fill in the following table (assuming a data length of 4). For correction/detection, only answer "Yes" if it can always correct/detect (and "No" if there is ever a case where the scheme can fail to correct/detect). We've filled in the first line for you.**


Table

| | 1-Repetition (No ECC) | 2-Repetition | 3-Repetition | Single Parity Bit | Hamming(7,4) |
|---|---|---|---|---|---|
| **Code Rate (Data Bits / Total Bits)** | 1.0 | 0.5 | 0.33 | 0.8 | 0.57 |
| **Max Number of Errors Can Detect** | 0 | 1 | 2 | 1 | 2 |
| **Max Number of Errors Can Correct** | 0 | 0 | 1 | 0 | 1 |


We know this because of the following reasons: 
- 2-Repetition (1011 1011): A single flip makes the two copies disagree, so you know an error occurred, but you can't tell which copy is correct — no correction possible.
- 3-Repetition (1011 1011 1011): Majority vote corrects 1 error. Two flips in the same bit position across two copies can be detected (the three copies won't all agree) but can't always be corrected.
- Single Parity Bit: XOR of all bits changes when any 1 bit flips — detectable. Two flips cancel out — undetectable, so max detection is 1. Can never correct since you don't know which bit flipped.
- Hamming(7,4): The overlapping parity circles let you pinpoint a single-bit error and correct it. Two errors are detectable (the syndrome is nonzero) but not correctable.

## 5-3

**When a single bit flip is detected, describe how Hamming(22,16) can correct this error.**


When a single-bit flip is detected (syndrome != 0 and overall parity = 1), the syndrome value
directly encodes the position of the flipped bit within the 22-bit encoded word. To correct
the error, flip that bit back by XORing the encoded word with a 1 at the position indicated
by the syndrome. This works because Hamming parity equations are made so that each
bit position produces a unique syndrome value.



## 5-5

**Can the Hamming(22,16) code we implemented always protect us from rowhammer attacks? If not, describe how a clever attacker could work around this scheme.**


No, Hamming(22,16) cannot always protect against Rowhammer. A clever attacker can work
around it in two ways:

- **Double bit flips in the same word**: Hamming(22,16) is SECDED — it can detect double
  errors but cannot correct them. If the attacker flips two bits within the same 22-bit
  protected word simultaneously, the system detects the error but cannot recover the original
  data, causing a crash or corruption.

- **Accumulated flips before a read**: ECC only corrects errors when the word is read. If
  the attacker can induce two flips in the same word before it is read (e.g., between memory
  scrubbing intervals), the double error goes uncorrectable. An attacker who understands the
  scrubbing interval can time their hammering to accumulate two flips within that window,
  bypassing ECC entirely.