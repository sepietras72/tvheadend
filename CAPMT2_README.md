# CAPMT2 — osobny klient OSCam dla TVHeadend

## Co to jest

Nowy, **niezależny** klient CA w TVHeadend: `CAPMT2 (OSCam Network, incremental list mgmt)`.
Nie zastępuje ani nie modyfikuje Twojego obecnego klienta `CAPMT` — działa obok niego jako
dodatkowa opcja do wyboru w konfiguracji. W każdej chwili możesz wrócić do starego klienta.

## Co jest w nim nowego

Stary `capmt.c` przy KAŻDEJ zmianie stanu jakiejś usługi (np. start drugiego kanału) wysyła
do OSCam **cały świeży CAPMT (LIST_FIRST → LIST_MORE → LIST_LAST) dla wszystkich aktywnych
kanałów na tym połączeniu** — również tych już poprawnie działających. To dokładnie
mechanizm, który omawialiśmy jako prawdopodobną przyczynę tego, że dołączenie drugiego
oglądanego programu potrafi "zatrząść" pierwszym.

`CAPMT2` implementuje prawdziwe przyrostowe zarządzanie listą wg specyfikacji CA_PMT:

- Pierwsze uruchomienie połączenia / wymuszone odświeżenie → pełna lista (LIST_FIRST/MORE/LAST), tak jak dotychczas.
- Dodanie KOLEJNEGO kanału do już działającego połączenia → wysyłany jest tylko pojedynczy
  komunikat **LIST_ADD** dla nowego kanału. Kanały już rozwiązane (`DS_RESOLVED`) w ogóle nie są
  ponownie dotykane.

Wartość `CAPMT_LIST_ADD` była w oryginalnym kodzie zdefiniowana, ale nigdzie faktycznie nieużywana —
to bezpieczna, zgodna ze specyfikacją zmiana zachowania, nie hack.

Dodatkowo `CAPMT2` loguje się pod osobnym podsystemem debugowania `capmt2`, więc jego logi
łatwo odróżnić od klasycznego `capmt` (Configuration → Debugging → zaznacz `capmt2`).

## Dlaczego dostajesz plik .patch, a nie gotowe pliki

Nie mam bezpośredniego dostępu zapisu do Twojego serwera z tej sesji (montowany dysk sieciowy
jest dla mnie tylko do odczytu) — dlatego przygotowałem gotowy, zweryfikowany patch, który
**już sprawdziłem, że aplikuje się czysto** (`git apply --check` na Twoim repo przeszło bez
błędów, jedyne ostrzeżenia dotyczą uprawnień plików i są nieistotne).

## Jak zastosować (na serwerze, przez Twoje SSH)

1. Pobierz oba pliki z czatu: `capmt2_feature.patch` i (dla wglądu) `capmt2.c`.
2. Skopiuj `capmt2_feature.patch` na serwer, do katalogu repo, np.:
   ```
   scp capmt2_feature.patch root@192.168.168.102:/root/test_compile/tvheadend/
   ```
3. Na serwerze:
   ```
   cd /root/test_compile/tvheadend
   git apply --check capmt2_feature.patch   # dry-run, powinno przejść bez błędów
   git apply capmt2_feature.patch
   ```
4. Przebuduj configure (żeby wygenerować `ENABLE_CAPMT2`) i skompiluj — Twoimi zwykłymi flagami,
   np.:
   ```
   ./configure --enable-libav --enable-libsvtav1 --enable-libsvtav1_static --enable-vaapi --enable-trace
   make
   ```
5. W interfejsie webowym TVHeadend: w sekcji konfiguracji klienta CA dodaj nowy wpis i wybierz
   z listy klas **„CAPMT2 (OSCam Network, incremental list mgmt)”**. Skonfiguruj go tymi samymi
   danymi co obecny CAPMT (adres/IP OSCam, port, tryb sieciowy, CW mode).
6. Włącz debug dla podsystemu `capmt2` (Configuration → Debugging) i przetestuj: odpal 2+ kanały
   naraz na różnych urządzeniach — w logu powinieneś zobaczyć linie
   `incremental ADD for service "..."` przy starcie drugiego kanału, zamiast pełnego resendu listy.

## Ważne zastrzeżenia

- **Nie kompilowałem ani nie testowałem tego kodu** — nie mam w tej sesji dostępu do kompilatora
  Twojego serwera. Traktuj to jako gotową do przetestowania propozycję, nie pewnik.
- Jeśli `make` zgłosi błąd kompilacji, wklej mi go tutaj — poprawię.
- Warto najpierw przetestować w momencie, gdy nie oglądacie ważnego programu na żywo.
