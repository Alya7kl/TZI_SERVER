#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>

using namespace std;

// Підключаємо клас для роботи з іменованими каналами
#include "PipeServer.h"

// За методичкою: сервер створює не менше 3 екземплярів каналу
#define MAX_PIPE_INST 3
#define PIPE_NAME "\\\\.\\pipe\\AuthPipe"

// Структура запиту від клієнта (Хакер має відправляти таку саму)
struct AuthMessage {
    char login[64];
    char password[64];
};

// Структура для зберігання користувачів у пам'яті
struct DBRecord {
    string login;
    string password;
    DWORD blockUntil; // Час, до якого запити відхиляються (для захисту)
};

int main()
{
    // Налаштування кирилиці в консолі
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    setlocale(LC_ALL, ".UTF8");

    // 1. Запит імені файлу з логінами та паролями (5 пар) через GetOpenFileName
    char szFile[260];
    for (int i = 0; i < 260; i++) {
        szFile[i] = 0; // Очищення масиву
    }

    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Text Files\0*.txt\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    cout << "Оберiть файл з паролями (наприклад, db.txt)..." << endl;
    if (GetOpenFileNameA(&ofn) == FALSE) {
        cout << "Файл не обрано. Завершення роботи." << endl;
        return 0;
    }

    // 2. Зчитування вмісту файлу
    ifstream db(szFile);
    if (db.is_open() == false) {
        cout << "Помилка вiдкриття файлу!" << endl;
        return 0;
    }

    // Перший рядок: максимальні довжини (за методичкою)
    int maxLoginLen, maxPassLen;
    db >> maxLoginLen >> maxPassLen;

    // Зчитуємо рівно 5 користувачів
    DBRecord users[5];
    for (int i = 0; i < 5; i++) {
        db >> users[i].login >> users[i].password;
        users[i].blockUntil = 0; // Поки що ніхто не заблокований
    }
    db.close();
    cout << "Успiшно завантажено 5 користувачiв." << endl;

    // 3. Вибір режиму роботи
    cout << "\nВиберiть режим роботи сервера:" << endl;
    cout << "1 - Звичайний режим" << endl;
    cout << "2 - Режим з протидiєю злому" << endl;

    int mode;
    cin >> mode;

    bool isProtected = false;
    if (mode == 2) {
        isProtected = true;
    }

    DWORD penaltyTime = 3000; // 3 секунди ігнорування при неправильному паролі

    HANDLE hEvents[MAX_PIPE_INST];
    CPipeServer<AuthMessage> Pipes[MAX_PIPE_INST];
    int PipesConnect = 0; // Лічильник підключених клієнтів

    // 4. Створення екземплярів каналу в асинхронному режимі
    for (int i = 0; i < MAX_PIPE_INST; i++) {
        hEvents[i] = CreateEvent(NULL, TRUE, TRUE, NULL);
        Pipes[i].CreatePipeAndWaitClient((char*)PIPE_NAME, hEvents[i], true, sizeof(AuthMessage));

        if (Pipes[i].GetState() == PIPE_ERROR) {
            cout << "Помилка створення каналу!" << endl;
            return 0;
        }
    }

    cout << "\nСервер запущено. Очiкування пiдключень..." << endl;

    // 5. Основний цикл обробки клієнтів
    while (true) {
        // Чекаємо завершення асинхронної операції
        DWORD waitResult = WaitForMultipleObjects(MAX_PIPE_INST, hEvents, FALSE, 5000);

        // Якщо пройшло 5 секунд і не було жодної активності від клієнтів
        if (waitResult == WAIT_TIMEOUT) {
            if (PipesConnect == 0) {
                int res = MessageBoxA(NULL, "Немає активних клієнтів більше 5 секунд!\nБажаєте продовжити роботу сервера?", "Повідомлення Адміністратору", MB_YESNO | MB_ICONQUESTION);

                if (res == IDNO) {
                    break;
                }
                else {
                    cout << "Продовжуємо роботу сервера..." << endl;
                }
            }
            continue; // Повертаємося до очікування
        }

        DWORD PipeNumber = waitResult - WAIT_OBJECT_0;

        if (PipeNumber < MAX_PIPE_INST) {
            DWORD NBytesRead;

            if (Pipes[PipeNumber].GetIOComplete() == false) {
                Pipes[PipeNumber].GetPendingResult(NBytesRead);
            }

            if (Pipes[PipeNumber].GetIOComplete() == true) {

                if (Pipes[PipeNumber].GetState() == PIPE_CONNECTED) {

                    if (Pipes[PipeNumber].GetOperState() == PIPE_JUST_CONNECTED) {
                        PipesConnect = PipesConnect + 1;
                    }

                    // Зчитуємо логін та пароль
                    AuthMessage msg;
                    if (Pipes[PipeNumber].ReadMessage(msg) == true) {

                        if (Pipes[PipeNumber].GetOperState() == PIPE_READ_SUCCESS) {

                            string rcvLogin = msg.login;
                            string rcvPass = msg.password;
                            char response = '0'; // За замовчуванням - авторизація неуспішна

                            // Шукаємо користувача в базі
                            int userIndex = -1;
                            for (int i = 0; i < 5; i++) {
                                if (users[i].login == rcvLogin) {
                                    userIndex = i;
                                    break;
                                }
                            }

                            DWORD curTime = GetTickCount(); // Поточний час

                            if (userIndex != -1) {
                                // Якщо увімкнено захист і час блокування ще не минув
                                if (isProtected == true && curTime < users[userIndex].blockUntil) {
                                    response = '0';
                                    cout << "Захист: iгнорування запиту для логiна " << rcvLogin << endl;
                                }
                                else {
                                    // Перевіряємо пароль
                                    if (users[userIndex].password == rcvPass) {
                                        response = '1'; // Пароль підійшов
                                        users[userIndex].blockUntil = 0; // Знімаємо блокування
                                        cout << "Успiшна авторизацiя: " << rcvLogin << endl;
                                    }
                                    else {
                                        response = '0'; // Пароль не підійшов
                                        cout << "Неправильний пароль для логiна: " << rcvLogin << endl;

                                        // Встановлюємо штрафний час для логіна
                                        if (isProtected == true) {
                                            users[userIndex].blockUntil = curTime + penaltyTime;
                                        }
                                    }
                                }
                            }
                            else {
                                cout << "Логiн не знайдено в базi: " << rcvLogin << endl;
                            }

                            // Відправляємо відповідь клієнту (1 або 0)
                            //Pipes[PipeNumber].WriteResponse(response);

                            //// Клієнт відключається сервером
                            //Pipes[PipeNumber].DisconnectClient();
                            //PipesConnect = PipesConnect - 1;

                            if (Pipes[PipeNumber].WriteResponse(response))
                            {
                                cout << "Відповідь відправлено клієнту: " << response << endl;
                            }
                            else
                            {
                                cout << "Помилка відправлення відповіді клієнту!" << endl;
                            }

                            Pipes[PipeNumber].DisconnectClient();
                            PipesConnect = PipesConnect - 1;

                            // 6. Якщо всі клієнти відключені, запитуємо через MessageBox
                            //if (PipesConnect == 0) {
                            //    int res = MessageBoxA(NULL, "Всi клiєнти вiдключенi!\nБажаєте продовжити роботу сервера?", "Повiдомлення Адмiнiстратору", MB_YESNO | MB_ICONQUESTION);

                            //    if (res == IDNO) {
                            //        // Якщо Ні - виходимо з нескінченного циклу
                            //        break;
                            //    }
                            //    else {
                            //        cout << "Продовжуємо роботу сервера..." << endl;
                            //    }
                            //}
                        }
                    }
                }
                else if (Pipes[PipeNumber].GetState() == PIPE_ERROR || Pipes[PipeNumber].GetState() == PIPE_LOST_CONNECT) {
                    if (PipesConnect > 0) {
                        PipesConnect = PipesConnect - 1;
                    }
                    Pipes[PipeNumber].DisconnectClient();
                    Pipes[PipeNumber].WaitClient();
                }
            }
        }
    }

    // Звільнення ресурсів перед виходом
    for (int i = 0; i < MAX_PIPE_INST; i++) {
        CloseHandle(hEvents[i]);
    }

    cout << "Роботу сервера завершено." << endl;
    return 0;
}