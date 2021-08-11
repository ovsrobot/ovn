/*
 * Copyright (c) 2021 Canonical
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef PLUG_DUMMY_H
#define PLUG_DUMMY_H 1

/*
 * The dummy plugger, allows for experimenting with plugging in a sandbox */

#ifdef  __cplusplus
extern "C" {
#endif

void plug_dummy_enable(void);

#ifdef  __cplusplus
}
#endif

#endif /* plug-dummy.h */
