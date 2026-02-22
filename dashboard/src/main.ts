import { createApp } from 'vue';
import { createRouter, createWebHistory } from 'vue-router';
import App from './App.vue';
import Overview from './pages/Overview.vue';
import DeviceDetail from './pages/DeviceDetail.vue';
import CameraGrid from './pages/CameraGrid.vue';
import Chat from './pages/Chat.vue';
import Rules from './pages/Rules.vue';
import ModelConversion from './pages/ModelConversion.vue';
import CameraManager from './pages/CameraManager.vue';
import ImageTester from './pages/ImageTester.vue';

const routes = [
  { path: '/', component: Overview },
  { path: '/device/:id', component: DeviceDetail },
  { path: '/cameras', component: CameraGrid },
  { path: '/chat', component: Chat },
  { path: '/rules', component: Rules },
  // Utility routes
  { path: '/utility/model-conversion', component: ModelConversion },
  { path: '/utility/camera-manager', component: CameraManager },
  { path: '/utility/image-tester', component: ImageTester },
];

const router = createRouter({
  history: createWebHistory(),
  routes,
});

const app = createApp(App);
app.use(router);
app.mount('#app');
