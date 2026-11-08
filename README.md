# ITFA-QR-CODE-ATTENDANCE-SYSTEM

EF101-P10 | EF-101 Fall 2026 | Course: ITFA

Group members:
-  Muhammad Ali Khan (EE-191)
-  Adeen Ikram (EE-196)
-  Syed Ramish Jamil (EE-184)

## About
A C++ console system in which a QR code links a student to an attendance
session. Records are stored in CSV files and used for reports.

## How to run
g++ -std=c++17 attendance.cpp -o attendance
./attendance

(or press Build and Run in Code::Blocks)

Admin password: admin123

## Features
- Student and course records
- Admin creates a session and a QR code is generated
- Student submits attendance with the QR text and ID
- Duplicate attendance is rejected
- Present/absent status, attendance percentage, class statistics
- Date and course filter
- Export to CSV (opens in Excel)
- Bonus: time-limited QR, admin login

## Limitations
Prototype only. The QR is shared by the whole class and CSV files are not encrypted.
