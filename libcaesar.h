#ifndef LIBCAESAR_H
#define LIBCAESAR_H

#ifdef __cplusplus
extern "C" {
#endif

// Функция инициализации защищённой области памяти для ключа
int init_secure_key(void);

// Установка ключа шифрования в защищённую область памяти
void set_key(char key);

// Получение текущего ключа из защищённой области памяти
char get_key(void);

// Шифрование/дешифрование данных с использованием XOR и защищённого ключа
void caesar(void* src, void* dst, int len);

// Очистка (затирание) защищённой области памяти с ключом и запрет доступа к ней
void clear_key(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBCAESAR_H */
